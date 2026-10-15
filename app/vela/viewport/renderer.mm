#include "renderer.h"

#include "pxr/imaging/cameraUtil/framing.h"
#include "pxr/imaging/glf/simpleLight.h"
#include "pxr/imaging/glf/simpleMaterial.h"
#include "pxr/imaging/hd/aov.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/renderSettings.h"
#include "pxr/imaging/hdx/pickTask.h"
#include "pxr/imaging/hdx/tokens.h"
#include "pxr/imaging/hgi/hgi.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/imaging/hgiMetal/hgi.h"
#include "pxr/imaging/hgiMetal/texture.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdRender/tokens.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"

#include <chrono>
#include <map>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vela {

namespace {

// Storm's AOVs are stored bottom row first; draw them upright.
NSString *const PresentShader = @R"(
#include <metal_stdlib>
using namespace metal;
struct Varyings { float4 position [[position]]; float2 uv; };
vertex Varyings velaPresentVertex(uint id [[vertex_id]]) {
    float2 p = float2((id << 1) & 2, id & 2);
    return { float4(p * 2.0 - 1.0, 0.0, 1.0), p };
}
fragment float4 velaPresentFragment(Varyings in [[stage_in]],
                                    texture2d<float> image [[texture(0)]]) {
    constexpr sampler nearest(filter::nearest, address::clamp_to_edge);
    return image.sample(nearest, in.uv);
}
)";

// Lighting and material defaults shared with usdview and usdrecord.
const GfVec4f SceneAmbient(0.01f, 0.01f, 0.01f, 1.0f);
const float ShininessDefault = 32.0f;

UsdImagingGLDrawMode HydraDrawMode(DrawMode mode) {
    switch (mode) {
    case DrawMode::Points: return UsdImagingGLDrawMode::DRAW_POINTS;
    case DrawMode::Wireframe: return UsdImagingGLDrawMode::DRAW_WIREFRAME;
    case DrawMode::WireframeOnSurface: return UsdImagingGLDrawMode::DRAW_WIREFRAME_ON_SURFACE;
    case DrawMode::ShadedFlat: return UsdImagingGLDrawMode::DRAW_SHADED_FLAT;
    case DrawMode::ShadedSmooth: return UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
    case DrawMode::GeomOnly: return UsdImagingGLDrawMode::DRAW_GEOM_ONLY;
    case DrawMode::GeomFlat: return UsdImagingGLDrawMode::DRAW_GEOM_FLAT;
    case DrawMode::GeomSmooth: return UsdImagingGLDrawMode::DRAW_GEOM_SMOOTH;
    }
    return UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
}

// As usdview sets them.
UsdImagingGLRenderParams MakeRenderParams(UsdTimeCode time, const ViewSettings &settings) {
    UsdImagingGLRenderParams params;
    params.frame = time;
    params.complexity = float(settings.complexity);
    params.drawMode = HydraDrawMode(settings.drawMode);
    params.colorCorrectionMode = TfToken(ToString(settings.colorCorrection));
    params.ocioDisplay = TfToken(settings.ocioDisplay);
    params.ocioView = TfToken(settings.ocioView);
    params.ocioColorSpace = TfToken(settings.ocioColorSpace);
    params.ocioLook = TfToken(settings.ocioLook);
    params.showProxy = settings.showProxy;
    params.showRender = settings.showRender;
    params.showGuides = settings.showGuides;
    params.cullStyle = settings.cullBackfaces
        ? UsdImagingGLCullStyle::CULL_STYLE_BACK_UNLESS_DOUBLE_SIDED
        : UsdImagingGLCullStyle::CULL_STYLE_NOTHING;
    params.enableSceneLights = settings.sceneLights;
    params.enableSceneMaterials = settings.sceneMaterials;
    return params;
}

TfToken ResolveRendererId(const std::string &name) {
    for (const TfToken &id : UsdImagingGLEngine::GetRendererPlugins()) {
        if (id.GetString() == name ||
            UsdImagingGLEngine::GetRendererDisplayName(id) == name) {
            return id;
        }
    }
    if (name == "Storm") return TfToken("HdStormRendererPlugin");
    return TfToken();
}

// Any JSON number: saved settings may come back as another numeric type.
double Number(const JsValue &value) {
    return value.IsReal() ? value.GetReal()
         : value.IsUInt64() ? double(value.GetUInt64()) : double(value.GetInt64());
}

VtValue ToVtValue(const RendererSetting::Type type, const JsValue &value) {
    switch (type) {
    case RendererSetting::Type::Flag: return VtValue(value.IsBool() && value.GetBool());
    case RendererSetting::Type::Int: return VtValue(int(Number(value)));
    case RendererSetting::Type::Float: return VtValue(float(Number(value)));
    case RendererSetting::Type::String:
        return VtValue(value.IsString() ? value.GetString() : std::string());
    }
    return VtValue();
}

JsValue ToJsValue(const VtValue &value) {
    if (value.IsHolding<bool>()) return JsValue(value.UncheckedGet<bool>());
    if (value.IsHolding<int>()) return JsValue(int64_t(value.UncheckedGet<int>()));
    if (value.IsHolding<float>()) return JsValue(double(value.UncheckedGet<float>()));
    if (value.IsHolding<double>()) return JsValue(value.UncheckedGet<double>());
    if (value.IsHolding<std::string>()) return JsValue(value.UncheckedGet<std::string>());
    if (value.IsHolding<TfToken>()) return JsValue(value.UncheckedGet<TfToken>().GetString());
    return JsValue();
}

RendererSetting::Type SettingType(UsdImagingGLRendererSetting::Type type) {
    switch (type) {
    case UsdImagingGLRendererSetting::TYPE_FLAG: return RendererSetting::Type::Flag;
    case UsdImagingGLRendererSetting::TYPE_INT: return RendererSetting::Type::Int;
    case UsdImagingGLRendererSetting::TYPE_FLOAT: return RendererSetting::Type::Float;
    case UsdImagingGLRendererSetting::TYPE_STRING: return RendererSetting::Type::String;
    }
    return RendererSetting::Type::String;
}

JsValue Lookup(const std::map<std::string, JsValue> &settings, const std::string &key) {
    auto found = settings.find(key);
    return found == settings.end() ? JsValue() : found->second;
}

} // namespace

struct Renderer::Impl {
    HgiUniquePtr hgi;
    HdDriver driver;
    std::unique_ptr<UsdImagingGLEngine> engine;
    UsdStageRefPtr stage;
    SdfPathVector selection;
    // The engine drops a selection set before its first render populates
    // the scene, so it is applied once populated.
    bool populated = false;
    bool selectionPending = false;
    id<MTLLibrary> library;
    std::unordered_map<NSUInteger, id<MTLRenderPipelineState>> pipelines;
    // Renderer choices applied to the engine; reset with the engine.
    TfToken defaultRenderer;
    std::string unknownRenderer;  // warned about once
    std::string appliedAov;
    std::optional<std::map<std::string, JsValue>> appliedSettings;
    std::optional<SdfPath> appliedSettingsPrim, appliedPassPrim;
    unsigned infoVersion = 0;

    HgiMetal *Metal() const { return static_cast<HgiMetal *>(hgi.get()); }
    id<MTLRenderPipelineState> Pipeline(MTLPixelFormat format);
    void ConfigureEngine();
    void ApplyRendererChoices(const ViewSettings &settings);
    void Render(const View &view);
};

// Settings the engine forgets when it switches renderer plugins (which
// rebuilds its task controller).
void Renderer::Impl::ConfigureEngine() {
    // Vela draws the AOV itself, so no OpenGL interop is needed.
    engine->SetEnablePresentation(false);
    engine->SetRendererAov(HdAovTokens->color);
    // usdview's default: a half-transparent yellow tint.
    engine->SetSelectionColor(GfVec4f(1.0f, 1.0f, 0.0f, 0.5f));
}

void Renderer::Impl::ApplyRendererChoices(const ViewSettings &s) {
    TfToken id = defaultRenderer;
    if (!s.renderer.empty()) {
        id = ResolveRendererId(s.renderer);
        if (id.IsEmpty()) {
            if (unknownRenderer != s.renderer) {
                TF_WARN("Unknown renderer '%s'", s.renderer.c_str());
                unknownRenderer = s.renderer;
            }
            id = engine->GetCurrentRendererId();
        }
    }
    if (id != engine->GetCurrentRendererId()) {
        if (engine->SetRendererPlugin(id)) {
            // A new render delegate: AOV, settings and selection again.
            ConfigureEngine();
            appliedAov = HdAovTokens->color.GetString();
            appliedSettings.reset();
            appliedSettingsPrim.reset();
            appliedPassPrim.reset();
            populated = false;
            selectionPending = true;
        } else {
            TF_WARN("Renderer '%s' is not supported", id.GetText());
        }
        ++infoVersion;
    }
    if (s.aov != appliedAov) {
        engine->SetRendererAov(TfToken(s.aov));
        appliedAov = s.aov;
    }
    if (!appliedSettings || *appliedSettings != s.rendererSettings) {
        for (const UsdImagingGLRendererSetting &setting : engine->GetRendererSettingsList()) {
            const std::string &key = setting.key.GetString();
            const JsValue now = Lookup(s.rendererSettings, key);
            const JsValue before = appliedSettings ? Lookup(*appliedSettings, key) : JsValue();
            if (now == before) continue;
            engine->SetRendererSetting(setting.key, now.IsNull() ? setting.defValue
                                       : ToVtValue(SettingType(setting.type), now));
        }
        appliedSettings = s.rendererSettings;
    }
    // As usdrecord: the stage's metadata when no settings prim is chosen.
    SdfPath settingsPrim = s.renderSettingsPrim;
    if (settingsPrim.IsEmpty()) {
        std::string path;
        if (stage->GetMetadata(UsdRenderTokens->renderSettingsPrimPath, &path) &&
            SdfPath::IsValidPathString(path)) {
            settingsPrim = SdfPath(path);
        }
    }
    if (appliedSettingsPrim != settingsPrim) {
        engine->SetActiveRenderSettingsPrimPath(settingsPrim);
        appliedSettingsPrim = settingsPrim;
    }
    if (appliedPassPrim != s.renderPassPrim) {
        engine->SetActiveRenderPassPrimPath(s.renderPassPrim);
        appliedPassPrim = s.renderPassPrim;
    }
}

id<MTLRenderPipelineState> Renderer::Impl::Pipeline(MTLPixelFormat format) {
    auto found = pipelines.find(format);
    if (found != pipelines.end()) {
        return found->second;
    }
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"velaPresentVertex"];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"velaPresentFragment"];
    descriptor.colorAttachments[0].pixelFormat = format;
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline =
        [Metal()->GetPrimaryDevice() newRenderPipelineStateWithDescriptor:descriptor
                                                                    error:&error];
    if (!pipeline) {
        TF_RUNTIME_ERROR("Vela present pipeline: %s",
                         error.localizedDescription.UTF8String);
    }
    pipelines[format] = pipeline;
    return pipeline;
}

void Renderer::Impl::Render(const View &view) {
    ApplyRendererChoices(view.settings);
    const GfFrustum frustum = view.camera.GetFrustum();
    if (view.cameraPath.IsEmpty()) {
        engine->SetCameraState(frustum.ComputeViewMatrix(),
                               frustum.ComputeProjectionMatrix());
    } else {
        engine->SetCameraPath(view.cameraPath);
    }
    engine->SetFraming(CameraUtilFraming(
        GfRect2i(GfVec2i(0), view.size[0], view.size[1])));
    engine->SetRenderBufferSize(view.size);

    // Default lights, for shaded draw modes only, as in usdview: a light at
    // the camera (also usdrecord's) and optionally a dome light.
    const ViewSettings &settings = view.settings;
    GlfSimpleLightVector lights;
    if (IsShaded(settings.drawMode) && settings.cameraLight) {
        const GfVec3d &position = frustum.GetPosition();
        GlfSimpleLight cameraLight(
            GfVec4f(position[0], position[1], position[2], 1.0f));
        cameraLight.SetTransform(frustum.ComputeViewInverse());
        cameraLight.SetAmbient(SceneAmbient);
        lights.push_back(cameraLight);
    }
    if (IsShaded(settings.drawMode) && settings.domeLight) {
        GlfSimpleLight dome;
        dome.SetIsDomeLight(true);
        if (UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z) {
            dome.SetTransform(GfMatrix4d().SetRotate(GfRotation(GfVec3d::XAxis(), 90)));
        }
        lights.push_back(dome);
    }
    GlfSimpleMaterial material;
    const float ambient = float(settings.defaultMaterialAmbient);
    const float specular = float(settings.defaultMaterialSpecular);
    material.SetAmbient(GfVec4f(ambient, ambient, ambient, 1.0f));
    material.SetSpecular(GfVec4f(specular, specular, specular, 1.0f));
    material.SetShininess(ShininessDefault);
    engine->SetLightingState(lights, material, SceneAmbient);
    engine->SetRendererSetting(HdRenderSettingsTokens->domeLightCameraVisibility,
                               VtValue(settings.domeLightTexturesVisible));

    UsdImagingGLRenderParams params = MakeRenderParams(view.time, settings);
    params.clearColor = view.clearColor;
    params.highlight = view.highlight;
    params.bboxes = view.bboxes;
    params.bboxLineColor = view.bboxColor;
    params.bboxLineDashSize = 3;  // usdview's
    if (!populated) {
        engine->Render(stage->GetPseudoRoot(), params);
        populated = true;
    }
    if (selectionPending) {
        engine->SetSelected(selection);
        selectionPending = false;
    }
    engine->Render(stage->GetPseudoRoot(), params);
}

Renderer::Renderer() : _impl(std::make_unique<Impl>()) {
    _impl->hgi = Hgi::CreatePlatformDefaultHgi();
    _impl->driver = HdDriver{HgiTokens->renderDriver, VtValue(_impl->hgi.get())};
    NSError *error = nil;
    _impl->library = [_impl->Metal()->GetPrimaryDevice()
        newLibraryWithSource:PresentShader options:nil error:&error];
    if (!_impl->library) {
        TF_RUNTIME_ERROR("Vela present shader: %s",
                         error.localizedDescription.UTF8String);
    }
}

Renderer::~Renderer() {
    // The engine uses the Hgi, so it goes first.
    _impl->engine.reset();
}

id<MTLDevice> Renderer::GetDevice() const {
    return _impl->Metal()->GetPrimaryDevice();
}

id<MTLCommandQueue> Renderer::GetQueue() const {
    return _impl->Metal()->GetQueue();
}

void Renderer::SetStage(const UsdStageRefPtr &stage, bool unloadedAsBounds) {
    _impl->engine.reset();
    _impl->stage = stage;
    if (!stage) {
        return;
    }
    UsdImagingGLEngine::Parameters params;
    params.driver = _impl->driver;
    params.gpuEnabled = true;
    params.displayUnloadedPrimsWithBounds = unloadedAsBounds;
    _impl->engine = std::make_unique<UsdImagingGLEngine>(params);
    _impl->ConfigureEngine();
    _impl->populated = false;
    _impl->selectionPending = !_impl->selection.empty();
    _impl->defaultRenderer = _impl->engine->GetCurrentRendererId();
    _impl->appliedAov = HdAovTokens->color.GetString();
    _impl->appliedSettings.reset();
    _impl->appliedSettingsPrim.reset();
    _impl->appliedPassPrim.reset();
    ++_impl->infoVersion;
}

RendererInfo Renderer::GetInfo() const {
    RendererInfo info;
    for (const TfToken &id : UsdImagingGLEngine::GetRendererPlugins()) {
        info.plugins.push_back({id.GetString(), UsdImagingGLEngine::GetRendererDisplayName(id)});
    }
    if (!_impl->engine) return info;
    info.current = _impl->engine->GetCurrentRendererId().GetString();
    for (const TfToken &aov : _impl->engine->GetRendererAovs()) {
        info.aovs.push_back(aov.GetString());
    }
    for (const UsdImagingGLRendererSetting &setting : _impl->engine->GetRendererSettingsList()) {
        // View > Dome Light Textures Visible owns this one; Render sets it.
        if (setting.key == HdRenderSettingsTokens->domeLightCameraVisibility) continue;
        info.settings.push_back({setting.key.GetString(), setting.name,
                                 SettingType(setting.type), ToJsValue(setting.defValue)});
    }
    return info;
}

unsigned Renderer::GetInfoVersion() const {
    return _impl->infoVersion;
}

std::string Renderer::ResolveRenderer(const std::string &name) {
    return ResolveRendererId(name).GetString();
}

void Renderer::SetSelection(const SdfPathVector &paths) {
    _impl->selection = paths;
    if (_impl->engine && _impl->populated) {
        _impl->engine->SetSelected(paths);
    } else {
        _impl->selectionPending = true;
    }
}

bool Renderer::Draw(const View &view, id<MTLTexture> target,
                    id<MTLCommandBuffer> commandBuffer) {
    if (!_impl->engine) {
        return false;
    }
    _impl->Render(view);
    // HgiMetal leaves render passes in its primary command buffer and
    // commits it only after a blit (color correction does one), so commit it
    // now: our pass, later on the same queue, then runs after Storm's.
    _impl->Metal()->CommitPrimaryCommandBuffer(
        HgiMetal::CommitCommandBuffer_NoWait, /*forceNewBuffer=*/true);
    HgiTextureHandle color = _impl->engine->GetAovTexture(HdAovTokens->color);
    auto *metalColor = static_cast<HgiMetalTexture *>(color.Get());
    id<MTLRenderPipelineState> pipeline = _impl->Pipeline(target.pixelFormat);
    if (!metalColor || !pipeline) {
        return false;
    }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:metalColor->GetTextureId() atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    return true;
}

std::string Renderer::GetRendererName() const {
    if (!_impl->engine) return std::string();
    return UsdImagingGLEngine::GetRendererDisplayName(_impl->engine->GetCurrentRendererId());
}

VtDictionary Renderer::GetRenderStats() const {
    return _impl->engine ? _impl->engine->GetRenderStats() : VtDictionary();
}

bool Renderer::IsConverged() const {
    return !_impl->engine || _impl->engine->IsConverged();
}

bool Renderer::WriteImage(const View &view, const std::string &path) {
    if (!_impl->engine) {
        return false;
    }
    _impl->engine->SetRendererSetting(
        HdRenderSettingsTokens->enableInteractive, VtValue(false));
    // As usdrecord does: render until converged before reading back.
    for (unsigned sleep = 10; ; sleep = std::min(100u, sleep + 5)) {
        _impl->Render(view);
        if (_impl->engine->IsConverged()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
    }
    const NSUInteger width = view.size[0], height = view.size[1];
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                     width:width height:height mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [GetDevice() newTextureWithDescriptor:descriptor];
    id<MTLBuffer> pixels = [GetDevice() newBufferWithLength:width * height * 4
                                                    options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commandBuffer = [GetQueue() commandBuffer];
    if (!Draw(view, target, commandBuffer)) {
        return false;
    }
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:pixels destinationOffset:0
    destinationBytesPerRow:width * 4 destinationBytesPerImage:width * height * 4];
    [blit endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    HioImage::StorageSpec storage;
    storage.width = int(width);
    storage.height = int(height);
    storage.format = HioFormatUNorm8Vec4;
    storage.flipped = false;
    storage.data = pixels.contents;
    HioImageSharedPtr image = HioImage::OpenForWriting(path);
    return image && image->Write(storage);
}

bool Renderer::Pick(const GfFrustum &frustum, UsdTimeCode time,
                    const ViewSettings &settings, SdfPath *path, GfVec3d *point) {
    if (!_impl->engine) {
        return false;
    }
    UsdImagingGLEngine::PickParams pickParams;
    pickParams.resolveMode = HdxPickTokens->resolveNearestToCenter;
    UsdImagingGLEngine::IntersectionResultVector results;
    if (!_impl->engine->TestIntersection(
            pickParams, frustum.ComputeViewMatrix(),
            frustum.ComputeProjectionMatrix(), _impl->stage->GetPseudoRoot(),
            MakeRenderParams(time, settings), &results) || results.empty()) {
        return false;
    }
    if (path) *path = results.front().hitPrimPath;
    if (point) *point = results.front().hitPoint;
    return true;
}

} // namespace vela
