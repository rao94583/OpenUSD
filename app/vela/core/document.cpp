#include "document.h"

#include "pxr/base/tf/errorMark.h"
#include "pxr/usd/usdGeom/bboxCache.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/tokens.h"

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
    _stageBoundsValid = false;
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
        _Notify(Selection);
    }
}

bool Document::IsAnimated() const {
    return _stage->HasAuthoredTimeCodeRange() &&
        _stage->GetEndTimeCode() > _stage->GetStartTimeCode();
}

double Document::GetStartTime() const { return _stage->GetStartTimeCode(); }
double Document::GetEndTime() const { return _stage->GetEndTimeCode(); }

double Document::GetFramesPerSecond() const {
    const double fps = _stage->GetFramesPerSecond();
    return fps > 0.0 ? fps : 24.0;
}

void Document::SetTime(UsdTimeCode time) {
    if (time != _time) {
        _time = time;
        _stageBoundsValid = false;
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

void Document::SetCamera(const FreeCamera &camera) {
    _camera = camera;
    _Notify(Camera);
}

void Document::CameraChanged() {
    _Notify(Camera);
}

GfBBox3d Document::ComputeBounds(const SdfPathVector &paths) const {
    UsdGeomBBoxCache cache(_time, {UsdGeomTokens->default_, UsdGeomTokens->proxy},
                           /* useExtentsHint = */ true);
    GfBBox3d bounds;
    for (const SdfPath &path : paths) {
        if (UsdPrim prim = _stage->GetPrimAtPath(path)) {
            bounds = GfBBox3d::Combine(bounds, cache.ComputeWorldBound(prim));
        }
    }
    return bounds;
}

const GfBBox3d &Document::GetStageBounds() const {
    if (!_stageBoundsValid) {
        _stageBounds = ComputeBounds({SdfPath::AbsoluteRootPath()});
        _stageBoundsValid = true;
    }
    return _stageBounds;
}

void Document::FrameAll() {
    if (_camera.Frame(GetStageBounds())) {
        _Notify(Camera);
    }
}

void Document::FrameSelection() {
    if (_selection.empty()) {
        FrameAll();
    } else if (_camera.Frame(ComputeBounds(_selection))) {
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
