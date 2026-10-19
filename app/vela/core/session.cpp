#include "session.h"
#include "document.h"
#include "image_jobs.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/usd/usdUtils/stageCache.h"

#include <algorithm>

namespace vela {

Session &Session::Get() {
    static Session *session = new Session;
    return *session;
}

int Session::Add(SessionDocument entry) {
    entry.id = _nextId++;
    if (entry.document) {
        entry.stageCacheId =
            UsdUtilsStageCache::Get().Insert(entry.document->GetStage()).ToLongInt();
    }
    _documents.push_back(std::move(entry));
    if (added) added(_documents.back());
    return _documents.back().id;
}

void Session::Remove(int id) {
    auto it = std::find_if(_documents.begin(), _documents.end(),
                           [id](const SessionDocument &d) { return d.id == id; });
    if (it == _documents.end()) return;
    if (it->stageCacheId) {
        UsdUtilsStageCache::Get().Erase(UsdStageCache::Id::FromLongInt(it->stageCacheId));
    }
    images::Service::Get().Close(id);
    if (removed) removed(id);
    _documents.erase(it);
    if (_front == id) _front = 0;
}

const SessionDocument *Session::Find(int id) const {
    for (const SessionDocument &entry : _documents) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

void Session::SetFront(int id) { _front = id; }

const SessionDocument *Session::GetCurrent() const {
    if (const SessionDocument *scoped = Find(_scoped)) return scoped;
    if (const SessionDocument *front = Find(_front)) return front;
    return _documents.empty() ? nullptr : &_documents.back();
}

Session::ScopedCurrent::ScopedCurrent(int id) : _previous(Get()._scoped) {
    Get()._scoped = id;
}

Session::ScopedCurrent::~ScopedCurrent() { Get()._scoped = _previous; }

namespace {

JsValue Vector(const GfVec3d &v) {
    return JsValue(JsArray{JsValue(v[0]), JsValue(v[1]), JsValue(v[2])});
}

JsValue PathOrNull(const SdfPath &path) {
    return path.IsEmpty() ? JsValue() : JsValue(path.GetString());
}

JsObject Header(const SessionDocument &entry) {
    const UsdStageRefPtr &stage = entry.document->GetStage();
    return JsObject{
        {"id", JsValue(entry.id)},
        {"name", JsValue(entry.name)},
        {"path", JsValue(stage->GetRootLayer()->GetRealPath())},
    };
}

} // namespace

JsObject DocumentState(const SessionDocument &entry) {
    const Document &d = *entry.document;
    JsObject state = Header(entry);
    JsArray selection;
    for (const SdfPath &path : d.GetSelection()) selection.push_back(JsValue(path.GetString()));
    state["selection"] = JsValue(selection);
    const UsdTimeCode time = d.GetTime();
    // Stages without animation sit at the earliest time.
    state["time"] = time.IsDefault() ? JsValue("default")
        : time.IsEarliestTime() ? JsValue("earliest") : JsValue(time.GetValue());
    state["startTime"] = JsValue(d.GetStartTime());
    state["endTime"] = JsValue(d.GetEndTime());
    state["framesPerSecond"] = JsValue(d.GetFramesPerSecond());
    state["animated"] = JsValue(d.IsAnimated());
    state["playing"] = JsValue(d.IsPlaying());

    const UsdStageRefPtr &stage = d.GetStage();
    state["editTarget"] = JsValue(d.GetEditTarget()->GetIdentifier());
    JsArray layers;
    for (const SdfLayerHandle &layer : stage->GetLayerStack(/* includeSessionLayers = */ true)) {
        layers.push_back(JsValue(JsObject{
            {"identifier", JsValue(layer->GetIdentifier())},
            {"dirty", JsValue(layer->IsDirty())},
            {"muted", JsValue(stage->IsLayerMuted(layer->GetIdentifier()))},
        }));
    }
    state["layers"] = JsValue(layers);
    state["unsavedEdits"] = JsValue(d.HasUnsavedEdits());

    // The viewport looks through a scene camera or the free camera.
    state["viewCamera"] = PathOrNull(d.GetViewCamera());
    const FreeCamera &camera = d.GetCamera();
    state["freeCamera"] = JsValue(JsObject{
        {"eye", Vector(camera.ComputeTransform().ExtractTranslation())},
        {"target", Vector(camera.GetCenter())},
        {"fov", JsValue(camera.GetFieldOfView())},
    });
    state["renderer"] = JsValue(d.GetRendererInfo().current);

    const UndoStack &undo = d.GetUndoStack();
    state["undo"] = undo.CanUndo() ? JsValue(undo.GetUndoName()) : JsValue();
    state["redo"] = undo.CanRedo() ? JsValue(undo.GetRedoName()) : JsValue();
    return state;
}

JsArray DocumentList() {
    const Session &session = Session::Get();
    const SessionDocument *current = session.GetCurrent();
    JsArray documents;
    for (const SessionDocument &entry : session.GetDocuments()) {
        JsObject header = Header(entry);
        header["current"] = JsValue(&entry == current);
        documents.push_back(JsValue(header));
    }
    return documents;
}

} // namespace vela
