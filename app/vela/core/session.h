// The app's open documents, for Python and MCP: each has an id, and one is
// current (the front window's, unless a script runs against another).
// The app registers documents as their windows open and supplies what
// only it can do, such as rendering a screenshot.
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

class Document;

struct SessionDocument {
    int id = 0;
    std::string name;  // the window title, e.g. "scene.usda"
    std::shared_ptr<Document> document;
    // Renders the viewport's view at width x height pixels (0 for the
    // viewport's size) as PNG data; returns false with a message in error.
    std::function<bool(int width, int height, std::vector<uint8_t> *png,
                       std::string *error)> screenshot;
    // Set by Session::Add.
    int64_t stageCacheId = 0;
};

class Session {
public:
    static Session &Get();

    // Adds a document and puts its stage in UsdUtilsStageCache (so Python
    // can find the same stage); returns its id. The newest is current until
    // another is made front.
    int Add(SessionDocument entry);
    void Remove(int id);
    const std::vector<SessionDocument> &GetDocuments() const { return _documents; }
    const SessionDocument *Find(int id) const;
    // The front window's document, for when no other is named.
    void SetFront(int id);
    // The current document: the scoped one (see ScopedCurrent), else the
    // front one, else the newest; null when none is open.
    const SessionDocument *GetCurrent() const;
    // Opens a file as the app does (File → Open), returning its id; set by
    // the app.
    std::function<int(const std::string &path, std::string *error)> open;

    // Makes a document current while it lives, e.g. for a console's script.
    class ScopedCurrent {
    public:
        explicit ScopedCurrent(int id);
        ~ScopedCurrent();
        ScopedCurrent(const ScopedCurrent &) = delete;
        ScopedCurrent &operator=(const ScopedCurrent &) = delete;
    private:
        int _previous;
    };

private:
    std::vector<SessionDocument> _documents;
    int _nextId = 1;
    int _front = 0;
    int _scoped = 0;
};

// What get_app_state reports about a document: selection, time, edit
// target, layers, camera, renderer and undo state.
JsObject DocumentState(const SessionDocument &entry);
// Every open document's id, name, path and whether it is current.
JsArray DocumentList();

} // namespace vela
