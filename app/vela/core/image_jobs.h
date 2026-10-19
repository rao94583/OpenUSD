// Provider-neutral image jobs. All methods and provider completions run on
// the main thread; providers do networking/decoding off that thread.
#pragma once
#include "pxr/pxr.h"
#include "pxr/base/js/value.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vela::images {
PXR_NAMESPACE_USING_DIRECTIVE
using Bytes = std::vector<uint8_t>;
std::string Text(const JsObject &, const std::string &, const std::string &fallback = "");
struct Result {
    JsObject value;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};
struct Output {
    Bytes png, preview;
    int width = 0, height = 0;
    std::string requestId, error, sha256;
    JsObject usage;
};
struct Model {
    std::string connection, provider, name, host;
    int maxReferences = 0;
    std::vector<std::string> sizes, qualities, backgrounds;
};
struct Request {
    std::string job, document, owner;
    Model model;
    JsObject options;
    std::vector<Bytes> references;
};
struct Provider {
    Model model;
    std::function<bool()> ready;
    // Returns a cancel function. Completion must be posted to the main thread.
    std::function<std::function<void()>(Request, std::function<void(Output)>)> submit;
};
class Service {
public:
    static Service &Get();
    void Configure(std::string folder);
    void Register(Provider provider);
    // key is a persisted UUID owned by the app's document association.
    void Bind(int document, const std::string &key);
    void Close(int document);
    void SetOwner(int document, std::string owner);
    void CancelOwner(int document, const std::string &owner);
    JsArray Models() const;
    Result Submit(int document, const JsObject &options);
    Result Approve(int document, const std::string &job, bool allow);
    Result Cancel(int document, const std::string &job);
    Result Job(int document, const std::string &job) const;
    JsArray Jobs(int document) const;
    Result AddReference(int document, Output output, const JsObject &source);
    Result Artifact(int document, const std::string &id) const;
    JsArray Artifacts(int document) const;
    Bytes Read(int document, const std::string &artifact, bool preview = false) const;
    // Called after transitions, e.g. to display approval/results. Never approves.
    std::function<void(int)> changed;
    std::function<Output(const Bytes &)> normalize;
    std::string Owner(int d) const { auto i = _owners.find(d); return i == _owners.end() ? "" : i->second; }
private:
    struct JobRecord {
        int session = 0;
        std::string document, owner, fingerprint;
        JsObject value;
        std::function<void()> cancel;
    };
    std::string _folder;
    std::map<int, std::string> _documents, _owners;
    std::map<std::string, Provider> _providers;
    std::map<std::string, JobRecord> _jobs;
    std::map<std::string, JsObject> _artifacts;
    unsigned _requests = 0;
    void Start(const std::string &job);
    void Pump();
    void Complete(const std::string &job, Output output);
    void Notify(int document);
    bool Save() const;
    std::string Key(int document) const;
};
} // namespace vela::images
