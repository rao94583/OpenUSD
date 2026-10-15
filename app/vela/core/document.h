// A document's state, with no UI: the stage and everything the app shows
// or changes about it. Views observe it; actions change it.
#pragma once

#include "free_camera.h"
#include "undo.h"
#include "view_settings.h"

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

// What the current renderer offers, published by the viewport (which owns
// the Hydra engine) for menus, actions and scripts.
struct RendererSetting {
    enum class Type { Flag, Int, Float, String };
    std::string key;
    std::string name;
    Type type = Type::Flag;
    JsValue defaultValue;

    bool operator==(const RendererSetting &other) const = default;
};

struct RendererInfo {
    struct Plugin {
        std::string id;
        std::string name;  // display name, e.g. "GL" for Storm
        bool operator==(const Plugin &other) const = default;
    };
    std::vector<Plugin> plugins;
    std::string current;  // the current plugin's id
    std::vector<std::string> aovs;
    std::vector<RendererSetting> settings;

    bool operator==(const RendererInfo &other) const = default;
};

// A UsdRenderPass prim's render settings prim (the first target of its
// renderSource relationship), or empty.
SdfPath RenderPassSource(const UsdStageRefPtr &stage, const SdfPath &pass);

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
        // With StageContent: prims were added, removed or recomposed (a USD
        // resync), not just property values or metadata changed.
        Resync = 1u << 7,
        // View settings: draw mode, purposes, lighting and the like.
        Display = 1u << 8,
        // Layers were saved or reverted (unsaved state changed without an
        // edit).
        Saved = 1u << 9,
        // The renderer's offerings (RendererInfo) changed.
        Renderer = 1u << 10,
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
    // Overrides the stage's time range for playback (usdview's --ff/--lf),
    // or returns to the stage's.
    void SetPlaybackRange(double start, double end);
    void ClearPlaybackRange();
    bool HasPlaybackRange() const { return _hasPlaybackRange; }
    // Moves by whole frames, wrapping around the time range.
    void StepTime(int frames);
    bool IsPlaying() const { return _playing; }
    void SetPlaying(bool playing);

    // The scene camera the viewport looks through, or empty for the free
    // camera. Setting one requires a UsdGeomCamera prim.
    const SdfPath &GetViewCamera() const { return _viewCamera; }
    bool SetViewCamera(const SdfPath &path, std::string *error = nullptr);
    // Switches to a free camera with the scene camera's current view, so
    // navigation never changes the scene's camera. Does nothing when
    // already on the free camera.
    void DetachViewCamera();
    // The camera to draw through for a viewport of the given aspect ratio:
    // the scene camera fitted to it (as usdview does), or the free camera.
    GfCamera ComputeViewGfCamera(double aspectRatio) const;
    // Scene cameras on the stage, in traversal order.
    SdfPathVector GetSceneCameras() const;

    // The free camera. Change it through GetCamera() and call CameraChanged.
    FreeCamera &GetCamera() { return _camera; }
    const FreeCamera &GetCamera() const { return _camera; }
    void SetCamera(const FreeCamera &camera);
    void CameraChanged();

    // World bounds of prims at the current time, for the default purpose and
    // the purposes the view settings show. The stage's bounds and the
    // selection's are cached.
    GfBBox3d ComputeBounds(const SdfPathVector &paths) const;
    const GfBBox3d &GetStageBounds() const;
    // The selection's bounds, or the stage's when nothing is selected.
    const GfBBox3d &GetSelectionBounds() const;
    // The viewport's aspect ratio (width / height), for framing.
    void SetViewportAspectRatio(double aspectRatio) { _aspectRatio = aspectRatio; }
    double GetViewportAspectRatio() const { return _aspectRatio; }
    void FrameAll();
    // Frames the selection, or everything when nothing is selected.
    void FrameSelection();

    const ViewSettings &GetViewSettings() const { return _viewSettings; }
    void SetViewSettings(const ViewSettings &settings);

    // Empty until a viewport has drawn the document.
    const RendererInfo &GetRendererInfo() const { return _rendererInfo; }
    void SetRendererInfo(const RendererInfo &info);
    // The setting's current value: the view settings' override or the
    // renderer's default. Null for unknown keys.
    JsValue GetRendererSetting(const std::string &key) const;
    // UsdRenderSettings and UsdRenderPass prims, in traversal order.
    SdfPathVector GetRenderSettingsPrims() const;
    SdfPathVector GetRenderPassPrims() const;

    // The layer edits go to: one in the stage's layer stack.
    SdfLayerHandle GetEditTarget() const;
    bool SetEditTarget(const SdfLayerHandle &layer, std::string *error = nullptr);

    // Edited layers that Save writes: the layer stack's dirty layers, except
    // the session layer and anonymous (in-memory) layers.
    SdfLayerHandleVector GetLayersToSave() const;
    // Whether any layer in the layer stack, other than the session layer,
    // has unsaved edits (including anonymous ones Save cannot write).
    bool HasUnsavedEdits() const;
    // Called after saving or reverting layers.
    void LayersSaved() { _Notify(Saved); }

    // How long opening the stage took and, from the start of opening, when
    // the first frame was drawn, in seconds (0 until known). The app
    // measures them; the HUD's Info shows them (usdview's --timing).
    struct LoadStatistics {
        double open = 0.0;
        double firstFrame = 0.0;
    };
    const LoadStatistics &GetLoadStatistics() const { return _loadStatistics; }
    void SetLoadStatistics(const LoadStatistics &statistics) { _loadStatistics = statistics; }

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
    bool _hasPlaybackRange = false;
    double _playbackStart = 0.0, _playbackEnd = 0.0;
    FreeCamera _camera;
    SdfPath _viewCamera;
    double _aspectRatio = 1.0;
    ViewSettings _viewSettings;
    RendererInfo _rendererInfo;
    LoadStatistics _loadStatistics;
    mutable GfBBox3d _stageBounds;
    mutable bool _stageBoundsValid = false;
    mutable GfBBox3d _selectionBounds;
    mutable bool _selectionBoundsValid = false;
    std::vector<std::pair<int, Observer>> _observers;
    int _nextObserver = 1;
    TfNotice::Key _objectsChangedKey;
};

} // namespace vela
