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
#include "pxr/usd/usd/prim.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"

#include <chrono>
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
const GfVec4f SpecularDefault(0.1f, 0.1f, 0.1f, 1.0f);
const GfVec4f AmbientDefault(0.2f, 0.2f, 0.2f, 1.0f);
const float ShininessDefault = 32.0f;

UsdImagingGLRenderParams MakeRenderParams(UsdTimeCode time) {
    UsdImagingGLRenderParams params;
    params.frame = time;
    params.complexity = 1.0f;
    params.colorCorrectionMode = HdxColorCorrectionTokens->sRGB;
    // usdview's and usdrecord's default purposes.
    params.showProxy = true;
    params.showRender = false;
    params.showGuides = false;
    return params;
}

} // namespace

struct Renderer::Impl {
    HgiUniquePtr hgi;
    HdDriver driver;
    std::unique_ptr<UsdImagingGLEngine> engine;
    UsdStageRefPtr stage;
    SdfPathVector selection;
    id<MTLLibrary> library;
    std::unordered_map<NSUInteger, id<MTLRenderPipelineState>> pipelines;

    HgiMetal *Metal() const { return static_cast<HgiMetal *>(hgi.get()); }
    id<MTLRenderPipelineState> Pipeline(MTLPixelFormat format);
    void Render(const View &view);
};

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

    // A light at the camera, as usdview and usdrecord use by default.
    const GfVec3d &position = frustum.GetPosition();
    GlfSimpleLight cameraLight(
        GfVec4f(position[0], position[1], position[2], 1.0f));
    cameraLight.SetTransform(frustum.ComputeViewInverse());
    cameraLight.SetAmbient(SceneAmbient);
    GlfSimpleMaterial material;
    material.SetAmbient(AmbientDefault);
    material.SetSpecular(SpecularDefault);
    material.SetShininess(ShininessDefault);
    engine->SetLightingState({cameraLight}, material, SceneAmbient);
    engine->SetRendererSetting(HdRenderSettingsTokens->domeLightCameraVisibility,
                               VtValue(view.domeLightsVisible));

    UsdImagingGLRenderParams params = MakeRenderParams(view.time);
    params.clearColor = view.clearColor;
    params.highlight = view.highlight;
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

void Renderer::SetStage(const UsdStageRefPtr &stage) {
    _impl->engine.reset();
    _impl->stage = stage;
    if (!stage) {
        return;
    }
    UsdImagingGLEngine::Parameters params;
    params.driver = _impl->driver;
    params.gpuEnabled = true;
    _impl->engine = std::make_unique<UsdImagingGLEngine>(params);
    // Vela draws the AOV itself, so no OpenGL interop is needed.
    _impl->engine->SetEnablePresentation(false);
    _impl->engine->SetRendererAov(HdAovTokens->color);
    // usdview's default: a half-transparent yellow tint.
    _impl->engine->SetSelectionColor(GfVec4f(1.0f, 1.0f, 0.0f, 0.5f));
    _impl->engine->SetSelected(_impl->selection);
}

void Renderer::SetSelection(const SdfPathVector &paths) {
    _impl->selection = paths;
    if (_impl->engine) {
        _impl->engine->SetSelected(paths);
    }
}

bool Renderer::Draw(const View &view, id<MTLTexture> target,
                    id<MTLCommandBuffer> commandBuffer) {
    if (!_impl->engine) {
        return false;
    }
    _impl->Render(view);
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
                    SdfPath *path, GfVec3d *point) {
    if (!_impl->engine) {
        return false;
    }
    UsdImagingGLEngine::PickParams pickParams;
    pickParams.resolveMode = HdxPickTokens->resolveNearestToCenter;
    UsdImagingGLEngine::IntersectionResultVector results;
    if (!_impl->engine->TestIntersection(
            pickParams, frustum.ComputeViewMatrix(),
            frustum.ComputeProjectionMatrix(), _impl->stage->GetPseudoRoot(),
            MakeRenderParams(time), &results) || results.empty()) {
        return false;
    }
    if (path) *path = results.front().hitPrimPath;
    if (point) *point = results.front().hitPoint;
    return true;
}

} // namespace vela
