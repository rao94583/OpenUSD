#include "undo.h"

#include "pxr/usd/sdf/changeBlock.h"
#include "pxr/usd/sdf/layerStateDelegate.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace vela {

namespace {

using Inverse = std::function<void()>;

struct Step {
    std::string name;
    std::vector<Inverse> inverses;
    // Order of creation, for Mark; undoing and redoing keep it.
    uint64_t serial = 0;
};

} // namespace

struct UndoStack::State {
    std::vector<Step> undo;
    std::vector<Step> redo;
    // The step edits are recorded into: an open group, an implicit step, or
    // the step an undo or redo is building.
    std::unique_ptr<Step> recording;
    int depth = 0;
    bool implicit = false;
    uint64_t nextSerial = 1;
    std::vector<SdfLayerHandle> layers;
    std::function<void()> changed;

    void Record(Inverse inverse) {
        if (!recording) {
            // An edit outside any group.
            recording = std::make_unique<Step>(Step{"Edit", {}});
            implicit = true;
        }
        recording->inverses.push_back(std::move(inverse));
    }

    // Ends an implicit step, if one is open.
    void FlushImplicit() {
        if (implicit) {
            implicit = false;
            Push(std::move(*recording));
            recording.reset();
        }
    }

    void Push(Step step) {
        if (step.inverses.empty()) {
            return;
        }
        if (!step.serial) step.serial = nextSerial++;
        undo.push_back(std::move(step));
        redo.clear();
        if (changed) changed();
    }

    // Applies a step's inverses newest first, recording their inverses
    // into the returned step.
    Step Apply(const Step &step) {
        recording = std::make_unique<Step>(Step{step.name, {}});
        {
            SdfChangeBlock block;
            for (auto it = step.inverses.rbegin(); it != step.inverses.rend(); ++it) {
                (*it)();
            }
        }
        Step result = std::move(*recording);
        result.serial = step.serial;
        recording.reset();
        return result;
    }
};

namespace {

// A layer's state delegate: tracks dirtiness, as SdfSimpleLayerStateDelegate
// does, and records the inverse of each edit before it is applied.
class Recorder : public SdfLayerStateDelegateBase {
public:
    explicit Recorder(std::weak_ptr<UndoStack::State> state)
        : _state(std::move(state)) {}

protected:
    bool _IsDirty() override { return _dirty; }
    void _MarkCurrentStateAsClean() override { _dirty = false; }
    void _MarkCurrentStateAsDirty() override { _dirty = true; }
    void _OnSetLayer(const SdfLayerHandle &layer) override {
        _layer = layer;
    }

    void _OnSetField(const SdfPath &path, const TfToken &field,
                     const VtValue &) override {
        RecordSetField(path, field);
    }
    void _OnSetField(const SdfPath &path, const TfToken &field,
                     const SdfAbstractDataConstValue &) override {
        RecordSetField(path, field);
    }

    void _OnSetFieldDictValueByKey(const SdfPath &path, const TfToken &field,
                                   const TfToken &keyPath,
                                   const VtValue &) override {
        RecordSetDictValue(path, field, keyPath);
    }
    void _OnSetFieldDictValueByKey(const SdfPath &path, const TfToken &field,
                                   const TfToken &keyPath,
                                   const SdfAbstractDataConstValue &) override {
        RecordSetDictValue(path, field, keyPath);
    }

    void _OnSetTimeSample(const SdfPath &path, double time,
                          const VtValue &) override {
        RecordSetTimeSample(path, time);
    }
    void _OnSetTimeSample(const SdfPath &path, double time,
                          const SdfAbstractDataConstValue &) override {
        RecordSetTimeSample(path, time);
    }

    void _OnCreateSpec(const SdfPath &path, SdfSpecType, bool inert) override {
        Record([path, inert](SdfLayerStateDelegateBase &self) {
            self.DeleteSpec(path, inert);
        });
    }

    // Deleting a spec erases its whole subtree, so save every spec and field
    // in it. Parents' child lists are restored by their own PopChild edits.
    void _OnDeleteSpec(const SdfPath &path, bool inert) override {
        struct Spec {
            SdfPath path;
            SdfSpecType type;
            std::vector<std::pair<TfToken, VtValue>> fields;
        };
        std::vector<Spec> specs;
        _layer->Traverse(path, [&](const SdfPath &p) {
            Spec spec{p, _layer->GetSpecType(p), {}};
            for (const TfToken &field : _layer->ListFields(p)) {
                spec.fields.emplace_back(field, _layer->GetField(p, field));
            }
            specs.push_back(std::move(spec));
        });
        // Traverse visits children first; recreate parents first.
        std::reverse(specs.begin(), specs.end());
        Record([specs = std::move(specs), inert](SdfLayerStateDelegateBase &self) {
            for (const Spec &spec : specs) {
                self.CreateSpec(spec.path, spec.type, inert);
                for (const auto &[field, value] : spec.fields) {
                    self.SetField(spec.path, field, value);
                }
            }
        });
    }

    void _OnMoveSpec(const SdfPath &oldPath, const SdfPath &newPath) override {
        Record([oldPath, newPath](SdfLayerStateDelegateBase &self) {
            self.MoveSpec(newPath, oldPath);
        });
    }

    void _OnPushChild(const SdfPath &parent, const TfToken &field,
                      const TfToken &value) override {
        Record([=](SdfLayerStateDelegateBase &self) {
            self.PopChild(parent, field, value);
        });
    }
    void _OnPushChild(const SdfPath &parent, const TfToken &field,
                      const SdfPath &value) override {
        Record([=](SdfLayerStateDelegateBase &self) {
            self.PopChild(parent, field, value);
        });
    }
    void _OnPopChild(const SdfPath &parent, const TfToken &field,
                     const TfToken &oldValue) override {
        Record([=](SdfLayerStateDelegateBase &self) {
            self.PushChild(parent, field, oldValue);
        });
    }
    void _OnPopChild(const SdfPath &parent, const TfToken &field,
                     const SdfPath &oldValue) override {
        Record([=](SdfLayerStateDelegateBase &self) {
            self.PushChild(parent, field, oldValue);
        });
    }

private:
    // Every edit dirties the layer, including undoing back to the saved
    // state, as with SdfSimpleLayerStateDelegate.
    void Record(std::function<void(SdfLayerStateDelegateBase &)> apply) {
        _dirty = true;
        std::shared_ptr<UndoStack::State> state = _state.lock();
        if (!state) {
            return;
        }
        // Inverses are applied through the layer's delegate, so that they
        // are recorded in turn (building the redo step) and notify Sdf.
        SdfLayerHandle layer = _layer;
        state->Record([layer, apply = std::move(apply)]() {
            if (layer) {
                if (SdfLayerStateDelegateBasePtr delegate = layer->GetStateDelegate()) {
                    apply(*delegate);
                }
            }
        });
    }

    void RecordSetField(const SdfPath &path, const TfToken &field) {
        // An empty old value erases the field again.
        VtValue old = _layer->GetField(path, field);
        Record([path, field, old](SdfLayerStateDelegateBase &self) {
            self.SetField(path, field, old);
        });
    }

    void RecordSetDictValue(const SdfPath &path, const TfToken &field,
                            const TfToken &keyPath) {
        VtValue old = _layer->GetFieldDictValueByKey(path, field, keyPath);
        Record([path, field, keyPath, old](SdfLayerStateDelegateBase &self) {
            self.SetFieldDictValueByKey(path, field, keyPath, old);
        });
    }

    void RecordSetTimeSample(const SdfPath &path, double time) {
        VtValue old;
        _layer->QueryTimeSample(path, time, &old);
        Record([path, time, old](SdfLayerStateDelegateBase &self) {
            self.SetTimeSample(path, time, old);
        });
    }

    std::weak_ptr<UndoStack::State> _state;
    SdfLayerHandle _layer;
    bool _dirty = false;
};


} // namespace

UndoStack::UndoStack() : _state(std::make_shared<State>()) {}

UndoStack::~UndoStack() {
    for (const SdfLayerHandle &layer : _state->layers) {
        if (layer) {
            // SetStateDelegate carries the layer's dirty state over.
            layer->SetStateDelegate(SdfSimpleLayerStateDelegate::New());
        }
    }
}

void UndoStack::Track(const SdfLayerHandle &layer) {
    if (!layer || IsTracking(layer)) {
        return;
    }
    layer->SetStateDelegate(TfCreateRefPtr(new Recorder(_state)));
    _state->layers.push_back(layer);
}

bool UndoStack::IsTracking(const SdfLayerHandle &layer) const {
    return std::find(_state->layers.begin(), _state->layers.end(), layer) !=
        _state->layers.end();
}

void UndoStack::BeginGroup(const std::string &name) {
    if (_state->depth++ == 0) {
        _state->FlushImplicit();
        _state->recording = std::make_unique<Step>(Step{name, {}});
    }
}

void UndoStack::EndGroup() {
    if (!TF_VERIFY(_state->depth > 0)) {
        return;
    }
    if (--_state->depth == 0) {
        Step step = std::move(*_state->recording);
        _state->recording.reset();
        _state->Push(std::move(step));
    }
}

uint64_t UndoStack::Mark() {
    if (_state->depth == 0) _state->FlushImplicit();
    return _state->nextSerial;
}

bool UndoStack::Coalesce(uint64_t mark, const std::string &name) {
    if (!TF_VERIFY(_state->depth == 0)) {
        return false;
    }
    _state->FlushImplicit();
    std::vector<Step> &undo = _state->undo;
    auto first = undo.end();
    while (first != undo.begin() && std::prev(first)->serial >= mark) --first;
    if (first == undo.end()) {
        return false;
    }
    Step merged{name, {}, first->serial};
    for (auto it = first; it != undo.end(); ++it) {
        for (Inverse &inverse : it->inverses) merged.inverses.push_back(std::move(inverse));
    }
    undo.erase(first, undo.end());
    undo.push_back(std::move(merged));
    if (_state->changed) _state->changed();
    return true;
}

bool UndoStack::Discard(uint64_t mark) {
    if (!TF_VERIFY(_state->depth == 0)) {
        return false;
    }
    _state->FlushImplicit();
    bool any = false;
    while (!_state->undo.empty() && _state->undo.back().serial >= mark) {
        Step step = std::move(_state->undo.back());
        _state->undo.pop_back();
        _state->Apply(step);
        any = true;
    }
    if (any && _state->changed) _state->changed();
    return any;
}

bool UndoStack::CanUndo() const {
    return _state->depth == 0 && (_state->implicit || !_state->undo.empty());
}

bool UndoStack::CanRedo() const {
    return _state->depth == 0 && !_state->implicit && !_state->redo.empty();
}

std::string UndoStack::GetUndoName() const {
    if (!CanUndo()) return std::string();
    return _state->implicit ? _state->recording->name : _state->undo.back().name;
}

std::string UndoStack::GetRedoName() const {
    return CanRedo() ? _state->redo.back().name : std::string();
}

bool UndoStack::Undo() {
    if (_state->depth > 0) {
        TF_CODING_ERROR("Undo inside an undo group");
        return false;
    }
    _state->FlushImplicit();
    if (_state->undo.empty()) {
        return false;
    }
    Step step = std::move(_state->undo.back());
    _state->undo.pop_back();
    _state->redo.push_back(_state->Apply(step));
    if (_state->changed) _state->changed();
    return true;
}

bool UndoStack::Redo() {
    if (_state->depth > 0) {
        TF_CODING_ERROR("Redo inside an undo group");
        return false;
    }
    _state->FlushImplicit();
    if (_state->redo.empty()) {
        return false;
    }
    Step step = std::move(_state->redo.back());
    _state->redo.pop_back();
    _state->undo.push_back(_state->Apply(step));
    if (_state->changed) _state->changed();
    return true;
}

void UndoStack::Clear() {
    _state->undo.clear();
    _state->redo.clear();
    if (_state->implicit) {
        _state->implicit = false;
        _state->recording.reset();
    }
    if (_state->changed) _state->changed();
}

void UndoStack::SetChangedCallback(std::function<void()> callback) {
    _state->changed = std::move(callback);
}

} // namespace vela
