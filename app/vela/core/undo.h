// Undo for USD. USD has none built in: an SdfLayerStateDelegateBase on each
// tracked layer records the inverse of every authoring operation (as Maya
// USD does), and inverses are grouped into undo steps.
#pragma once

#include "pxr/pxr.h"
#include "pxr/usd/sdf/layer.h"

#include <functional>
#include <memory>
#include <string>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

class UndoStack {
public:
    UndoStack();
    // Returns tracked layers to plain state delegates, keeping their dirty
    // state.
    ~UndoStack();
    UndoStack(const UndoStack &) = delete;
    UndoStack &operator=(const UndoStack &) = delete;

    // Records edits to the layer from now on. Does nothing if it is already
    // tracked.
    void Track(const SdfLayerHandle &layer);
    bool IsTracking(const SdfLayerHandle &layer) const;

    // Edits between BeginGroup and EndGroup become one undo step named
    // name. Nested groups fold into the outermost one. Edits made outside
    // a group form a step named "Edit", ended by the next group, undo or
    // redo.
    void BeginGroup(const std::string &name);
    void EndGroup();

    bool CanUndo() const;
    bool CanRedo() const;
    // The names of the steps Undo and Redo would apply, or "".
    std::string GetUndoName() const;
    std::string GetRedoName() const;
    // Reverts the last step, or reapplies the last undone one. Returns
    // false if there is nothing to do.
    bool Undo();
    bool Redo();
    void Clear();

    // Called after the stacks change: a step was added, undone, redone or
    // cleared.
    void SetChangedCallback(std::function<void()> callback);

    struct State;

private:
    std::shared_ptr<State> _state;
};

// Groups the edits made during its lifetime into one undo step.
class UndoBlock {
public:
    UndoBlock(UndoStack &stack, const std::string &name) : _stack(stack) {
        _stack.BeginGroup(name);
    }
    ~UndoBlock() { _stack.EndGroup(); }
    UndoBlock(const UndoBlock &) = delete;
    UndoBlock &operator=(const UndoBlock &) = delete;

private:
    UndoStack &_stack;
};

} // namespace vela
