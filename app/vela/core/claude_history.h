// The Claude panel's conversation history: the panel's own log of each
// conversation (prompts, replies, tool calls and results), one JSON object
// per line in <folder>/<session id>.jsonl. The first line describes the
// conversation: {"kind": "conversation", "title", "document", "created"}.
// The CLI keeps its own transcript for resuming; this one is for showing.
#pragma once

#include "pxr/pxr.h"
#include "pxr/base/js/value.h"

#include <string>
#include <vector>

namespace vela {

PXR_NAMESPACE_USING_DIRECTIVE

namespace claude {

struct Conversation {
    std::string id;        // the CLI's session id
    std::string title;     // the first request, shortened
    std::string document;  // the document's file path ("" if unsaved)
    double created = 0.0;  // seconds since 1970
    double updated = 0.0;  // the log's modification time
};

// ~/Library/Application Support/Vela/Claude, or VELA_CLAUDE_HISTORY when
// set (for tests).
std::string DefaultHistoryFolder();

// Starts a conversation's log; does nothing if it exists.
bool CreateConversation(const std::string &folder, const Conversation &conversation,
                        std::string *error = nullptr);
// Appends an entry to a conversation's log.
bool AppendEntry(const std::string &folder, const std::string &id, const JsObject &entry);
// A conversation's entries, after the first line.
std::vector<JsObject> LoadEntries(const std::string &folder, const std::string &id);
// The conversations about document (all of them when it is ""), newest
// first.
std::vector<Conversation> ListConversations(const std::string &folder,
                                            const std::string &document = std::string());
bool DeleteConversation(const std::string &folder, const std::string &id);

// A conversation's title from its first request.
std::string TitleFor(const std::string &request);

} // namespace claude
} // namespace vela
