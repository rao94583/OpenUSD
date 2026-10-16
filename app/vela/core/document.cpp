#include "document.h"

#include "pxr/base/gf/frustum.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/cameraUtil/conformWindow.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/bboxCache.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdRender/pass.h"
#include "pxr/usd/usdRender/settings.h"

#include <algorithm>
#include <cmath>

namespace vela {

std::shared_ptr<Document> Document::Open(const std::string &path,
                                         std::string *error) {
    TfErrorMark mark;
    UsdStageRefPtr stage = UsdStage::Open(path);
    if (!stage) {
        if (error) {
            *error = "Could not open USD stage: " + path;
            for (const TfError &e : mark) {
                *error += "\n" + e.GetCommentary();
            }
        }
        mark.Clear();
        return nullptr;
    }
    return std::make_shared<Document>(stage);
}

Document::Document(const UsdStageRefPtr &stage)
    : _stage(stage),
      _undo(std::make_unique<UndoStack>()),
      _camera(UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z)
{
    _time = IsAnimated() ? UsdTimeCode(GetStartTime()) : UsdTimeCode::EarliestTime();
    _TrackLayers();
    _undo->SetChangedCallback([this] { _Notify(UndoState); });
    _objectsChangedKey = TfNotice::Register(
        TfCreateWeakPtr(this), &Document::_OnObjectsChanged, UsdStageWeakPtr(stage));
    FrameAll();
}

Document::~Document() {
    TfNotice::Revoke(_objectsChangedKey);
    _undo->SetChangedCallback(nullptr);
}

int Document::AddObserver(Observer observer) {
    _observers.emplace_back(_nextObserver, std::move(observer));
    return _nextObserver++;
}

void Document::RemoveObserver(int id) {
    _observers.erase(std::remove_if(_observers.begin(), _observers.end(),
                                    [id](const auto &o) { return o.first == id; }),
                     _observers.end());
}

void Document::_Notify(unsigned changes) {
    // Copied, so observers may add or remove observers.
    const auto observers = _observers;
    for (const auto &[id, observer] : observers) {
        observer(changes);
    }
}

void Document::_OnObjectsChanged(const UsdNotice::ObjectsChanged &notice,
                                 const UsdStageWeakPtr &) {
    _stageBoundsValid = _selectionBoundsValid = false;
    // Recomposition, or a change to layer metadata such as subLayers, may
    // bring in layers. (An empty new sublayer resyncs nothing.)
    const auto &infoPaths = notice.GetChangedInfoOnlyPaths();
    if (!notice.GetResyncedPaths().empty() ||
        std::find(infoPaths.begin(), infoPaths.end(),
                  SdfPath::AbsoluteRootPath()) != infoPaths.end()) {
        _TrackLayers();
    }
    if (notice.GetResyncedPaths().empty()) {
        _Notify(StageContent);
        return;
    }
    // Drop selected prims that no longer exist.
    SdfPathVector selection;
    for (const SdfPath &path : _selection) {
        if (_stage->GetPrimAtPath(path)) selection.push_back(path);
    }
    unsigned changes = StageContent | Resync;
    if (!_viewCamera.IsEmpty() && !UsdGeomCamera(_stage->GetPrimAtPath(_viewCamera))) {
        _viewCamera = SdfPath();
        changes |= Camera;
    }
    if (selection != _selection) {
        _selection = selection;
        changes |= Selection;
    }
    _Notify(changes);
}

// Every layer the stage uses can be edited, through an edit target or
// directly, so all of them record undo.
void Document::_TrackLayers() {
    for (const SdfLayerHandle &layer : _stage->GetUsedLayers()) {
        _undo->Track(layer);
    }
}

void Document::SetSelection(const SdfPathVector &paths) {
    SdfPathVector selection;
    for (const SdfPath &path : paths) {
        if (std::find(selection.begin(), selection.end(), path) == selection.end()) {
            selection.push_back(path);
        }
    }
    if (selection != _selection) {
        _selection = std::move(selection);
        _selectionBoundsValid = false;
        _Notify(Selection);
    }
}

bool Document::IsAnimated() const {
    return (_hasPlaybackRange || _stage->HasAuthoredTimeCodeRange()) &&
        GetEndTime() > GetStartTime();
}

double Document::GetStartTime() const {
    return _hasPlaybackRange ? _playbackStart : _stage->GetStartTimeCode();
}
double Document::GetEndTime() const {
    return _hasPlaybackRange ? _playbackEnd : _stage->GetEndTimeCode();
}

void Document::SetPlaybackRange(double start, double end) {
    _hasPlaybackRange = true;
    _playbackStart = start;
    _playbackEnd = end;
    _Notify(Time);
}

void Document::ClearPlaybackRange() {
    _hasPlaybackRange = false;
    _Notify(Time);
}

double Document::GetFramesPerSecond() const {
    const double fps = _stage->GetFramesPerSecond();
    return fps > 0.0 ? fps : 24.0;
}

void Document::SetTime(UsdTimeCode time) {
    if (time != _time) {
        _time = time;
        _stageBoundsValid = _selectionBoundsValid = false;
        _Notify(Time);
    }
}

void Document::StepTime(int frames) {
    if (!IsAnimated()) {
        return;
    }
    const double start = GetStartTime(), end = GetEndTime();
    const double length = std::floor(end - start) + 1.0;
    const double current = _time.IsNumeric() ? _time.GetValue() : start;
    double offset = std::fmod(std::round(current - start) + frames, length);
    if (offset < 0.0) offset += length;
    SetTime(UsdTimeCode(start + offset));
}

void Document::SetPlaying(bool playing) {
    playing = playing && IsAnimated();
    if (playing != _playing) {
        _playing = playing;
        _Notify(Playback);
    }
}

bool Document::SetViewCamera(const SdfPath &path, std::string *error) {
    if (!path.IsEmpty() && !UsdGeomCamera(_stage->GetPrimAtPath(path))) {
        if (error) *error = "No camera at " + path.GetString();
        return false;
    }
    if (path != _viewCamera) {
        _viewCamera = path;
        _Notify(Camera);
    }
    return true;
}

void Document::DetachViewCamera() {
    if (_viewCamera.IsEmpty()) {
        return;
    }
    const GfCamera camera = ComputeViewGfCamera(_aspectRatio);
    // Without a focus distance, the center of interest goes at the depth of
    // the stage's center.
    const GfVec3d center = GetStageBounds().ComputeCentroid();
    const GfFrustum frustum = camera.GetFrustum();
    double distance = GfDot(center - frustum.GetPosition(), frustum.ComputeViewDirection());
    if (!(distance > 0.0)) distance = 1.0;
    _camera = FreeCamera::FromGfCamera(camera, _camera.IsZUp(), distance);
    _viewCamera = SdfPath();
    _Notify(Camera);
}

GfCamera Document::ComputeViewGfCamera(double aspectRatio) const {
    return ComputeViewGfCamera(aspectRatio, _time);
}

GfCamera Document::ComputeViewGfCamera(double aspectRatio, UsdTimeCode time) const {
    if (UsdGeomCamera camera{_stage->GetPrimAtPath(_viewCamera)}) {
        GfCamera result = camera.GetCamera(time);
        // Fit the camera's whole aperture inside the viewport.
        CameraUtilConformWindow(&result, CameraUtilFit, aspectRatio > 0.0 ? aspectRatio : 1.0);
        return result;
    }
    return _camera.ComputeGfCamera(aspectRatio, GetStageBounds());
}

double Document::GetViewCameraAspectRatio(UsdTimeCode time) const {
    if (UsdGeomCamera camera{_stage->GetPrimAtPath(_viewCamera)}) {
        const float aspect = camera.GetCamera(time).GetAspectRatio();
        if (aspect > 0.0f) return aspect;
    }
    return _aspectRatio > 0.0 ? _aspectRatio : 1.0;
}

const std::vector<std::string> &ImageExportExtensions() {
    // What Hio's plugins in the bundle write: stb, ImageIO and OpenEXR.
    static const std::vector<std::string> extensions = {
        "png", "jpg", "jpeg", "tif", "tiff", "exr", "bmp", "tga"};
    return extensions;
}

SdfLayerHandleVector Document::GetLayersToSave() const {
    SdfLayerHandleVector layers;
    for (const SdfLayerHandle &layer : _stage->GetLayerStack(/* includeSessionLayers = */ false)) {
        if (layer->IsDirty() && !layer->IsAnonymous()) layers.push_back(layer);
    }
    return layers;
}

bool Document::HasUnsavedEdits() const {
    for (const SdfLayerHandle &layer : _stage->GetLayerStack(false)) {
        if (layer->IsDirty()) return true;
    }
    return false;
}

SdfPathVector Document::GetSceneCameras() const {
    SdfPathVector cameras;
    for (const UsdPrim &prim : _stage->Traverse()) {
        if (prim.IsA<UsdGeomCamera>()) cameras.push_back(prim.GetPath());
    }
    return cameras;
}

void Document::SetViewSettings(const ViewSettings &settings) {
    if (!(settings == _viewSettings)) {
        // Bounds follow the purposes shown.
        _stageBoundsValid = _selectionBoundsValid = false;
        _viewSettings = settings;
        _Notify(Display);
    }
}

SdfPath RenderPassSource(const UsdStageRefPtr &stage, const SdfPath &pass) {
    SdfPathVector targets;
    if (UsdRenderPass renderPass{stage->GetPrimAtPath(pass)}) {
        renderPass.GetRenderSourceRel().GetForwardedTargets(&targets);
    }
    return targets.empty() ? SdfPath() : targets.front();
}

void Document::SetValidationIssues(std::vector<ValidationIssue> issues) {
    _validationIssues = std::move(issues);
    _validated = true;
    _Notify(Validation);
}

void Document::SetRendererInfo(const RendererInfo &info) {
    if (!(info == _rendererInfo)) {
        _rendererInfo = info;
        _Notify(Renderer);
    }
}

JsValue Document::GetRendererSetting(const std::string &key) const {
    auto found = _viewSettings.rendererSettings.find(key);
    if (found != _viewSettings.rendererSettings.end()) return found->second;
    for (const RendererSetting &setting : _rendererInfo.settings) {
        if (setting.key == key) return setting.defaultValue;
    }
    return JsValue();
}

SdfPathVector Document::GetRenderSettingsPrims() const {
    SdfPathVector paths;
    for (const UsdPrim &prim : _stage->Traverse()) {
        if (prim.IsA<UsdRenderSettings>()) paths.push_back(prim.GetPath());
    }
    return paths;
}

SdfPathVector Document::GetRenderPassPrims() const {
    SdfPathVector paths;
    for (const UsdPrim &prim : _stage->Traverse()) {
        if (prim.IsA<UsdRenderPass>()) paths.push_back(prim.GetPath());
    }
    return paths;
}

void Document::SetCamera(const FreeCamera &camera) {
    _camera = camera;
    _Notify(Camera);
}

void Document::CameraChanged() {
    _Notify(Camera);
}

GfBBox3d Document::ComputeBounds(const SdfPathVector &paths) const {
    TfTokenVector purposes{UsdGeomTokens->default_};
    if (_viewSettings.showProxy) purposes.push_back(UsdGeomTokens->proxy);
    if (_viewSettings.showRender) purposes.push_back(UsdGeomTokens->render);
    if (_viewSettings.showGuides) purposes.push_back(UsdGeomTokens->guide);
    UsdGeomBBoxCache cache(_time, purposes, /* useExtentsHint = */ true);
    GfBBox3d bounds;
    for (const SdfPath &path : paths) {
        if (UsdPrim prim = _stage->GetPrimAtPath(path)) {
            bounds = GfBBox3d::Combine(bounds, cache.ComputeWorldBound(prim));
        }
    }
    return bounds;
}

const GfBBox3d &Document::GetSelectionBounds() const {
    if (_selection.empty()) return GetStageBounds();
    if (!_selectionBoundsValid) {
        _selectionBounds = ComputeBounds(_selection);
        _selectionBoundsValid = true;
    }
    return _selectionBounds;
}

const GfBBox3d &Document::GetStageBounds() const {
    if (!_stageBoundsValid) {
        _stageBounds = ComputeBounds({SdfPath::AbsoluteRootPath()});
        _stageBoundsValid = true;
    }
    return _stageBounds;
}

void Document::FrameAll() {
    DetachViewCamera();
    if (_camera.Frame(GetStageBounds(), 1.1, _aspectRatio)) {
        _Notify(Camera);
    }
}

void Document::FrameSelection() {
    DetachViewCamera();
    if (_selection.empty()) {
        FrameAll();
    } else if (_camera.Frame(ComputeBounds(_selection), 1.1, _aspectRatio)) {
        _Notify(Camera);
    }
}

SdfLayerHandle Document::GetEditTarget() const {
    return _stage->GetEditTarget().GetLayer();
}

bool Document::SetEditTarget(const SdfLayerHandle &layer, std::string *error) {
    if (!layer || !_stage->HasLocalLayer(layer)) {
        if (error) {
            *error = "Not a layer in the stage's layer stack: " +
                (layer ? layer->GetIdentifier() : std::string("(none)"));
        }
        return false;
    }
    if (layer != GetEditTarget()) {
        _stage->SetEditTarget(UsdEditTarget(layer));
        _Notify(EditTarget);
    }
    return true;
}

} // namespace vela
