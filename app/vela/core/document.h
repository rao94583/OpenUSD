// A document's state, with no UI: the stage and everything the app shows
// or changes about it. Views observe it; actions change it.
#pragma once

#include "free_camera.h"
#include "undo.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/bbox3d.h"
#include "pxr/base/tf/weakBase.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/notice.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/timeCode.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

class Document : public TfWeakBase {
public:
    // What changed, for observers.
    enum Change : unsigned {
        StageContent = 1u << 0,
        Selection = 1u << 1,
        Time = 1u << 2,
        Camera = 1u << 3,
        EditTarget = 1u << 4,
        UndoState = 1u << 5,
        Playback = 1u << 6,
    };
    using Observer = std::function<void(unsigned changes)>;

    // Opens the stage at path, or returns null with a message in error.
    static std::shared_ptr<Document> Open(const std::string &path,
                                          std::string *error = nullptr);
    explicit Document(const UsdStageRefPtr &stage);
    ~Document();
    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;

    const UsdStageRefPtr &GetStage() const { return _stage; }

    // Observers are called after each change, on the calling thread.
    int AddObserver(Observer observer);
    void RemoveObserver(int id);

    // Selection: prim paths, in selection order, without duplicates.
    const SdfPathVector &GetSelection() const { return _selection; }
    void SetSelection(const SdfPathVector &paths);

    // Time. Stages without an authored time code range have one frame.
    UsdTimeCode GetTime() const { return _time; }
    void SetTime(UsdTimeCode time);
    bool IsAnimated() const;
    double GetStartTime() const;
    double GetEndTime() const;
    double GetFramesPerSecond() const;
    // Moves by whole frames, wrapping around the time range.
    void StepTime(int frames);
    bool IsPlaying() const { return _playing; }
    void SetPlaying(bool playing);

    // The free camera. Change it through GetCamera() and call CameraChanged.
    FreeCamera &GetCamera() { return _camera; }
    const FreeCamera &GetCamera() const { return _camera; }
    void SetCamera(const FreeCamera &camera);
    void CameraChanged();

    // World bounds of prims at the current time, with the default and
    // proxy purposes. The stage's bounds are cached.
    GfBBox3d ComputeBounds(const SdfPathVector &paths) const;
    const GfBBox3d &GetStageBounds() const;
    void FrameAll();
    // Frames the selection, or everything when nothing is selected.
    void FrameSelection();

    // The layer edits go to: one in the stage's layer stack.
    SdfLayerHandle GetEditTarget() const;
    bool SetEditTarget(const SdfLayerHandle &layer, std::string *error = nullptr);

    UndoStack &GetUndoStack() { return *_undo; }
    const UndoStack &GetUndoStack() const { return *_undo; }

private:
    void _Notify(unsigned changes);
    void _OnObjectsChanged(const UsdNotice::ObjectsChanged &notice,
                           const UsdStageWeakPtr &sender);
    void _TrackLayers();

    UsdStageRefPtr _stage;
    std::unique_ptr<UndoStack> _undo;
    SdfPathVector _selection;
    UsdTimeCode _time;
    bool _playing = false;
    FreeCamera _camera;
    mutable GfBBox3d _stageBounds;
    mutable bool _stageBoundsValid = false;
    std::vector<std::pair<int, Observer>> _observers;
    int _nextObserver = 1;
    TfNotice::Key _objectsChangedKey;
};

} // namespace vela
