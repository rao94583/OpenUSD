#include "image_jobs.h"
#include "app_settings.h"
#include "mcp.h"
#include "pxr/base/js/json.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sys/stat.h>

namespace vela::images {
namespace fs = std::filesystem;
std::string Text(const JsObject &o, const std::string &k, const std::string &fallback) {
    auto it = o.find(k);
    return it != o.end() && it->second.IsString() ? it->second.GetString() : fallback;
}
namespace {
std::string Id() {
    static std::mt19937_64 random(std::random_device{}());
    return std::to_string(random()) + "-" + std::to_string(random());
}
Result Error(std::string s) { return {{}, std::move(s)}; }
bool Terminal(const std::string &s) { return s == "succeeded" || s == "failed" || s == "cancelled"; }
Bytes Load(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() < 0 || f.tellg() > 32 * 1024 * 1024) return {};
    Bytes b(size_t(f.tellg())); f.seekg(0); f.read(reinterpret_cast<char *>(b.data()), b.size());
    return f ? b : Bytes();
}
bool Write(const std::string &path, const Bytes &bytes) {
    std::ofstream f(path + ".tmp", std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    f.close();
    if (!f) return false;
    chmod((path + ".tmp").c_str(), 0600);
    std::error_code ec; fs::rename(path + ".tmp", path, ec); return !ec;
}
bool Contains(const std::vector<std::string> &values, const std::string &v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}
JsArray Strings(const std::vector<std::string> &values) {
    JsArray a; for (const auto &s : values) a.push_back(JsValue(s)); return a;
}
}
Service &Service::Get() { static Service *s = new Service; return *s; }
void Service::Configure(std::string folder) {
    _folder = std::move(folder);
    std::error_code ec; fs::create_directories(_folder, ec); chmod(_folder.c_str(), 0700);
    const Bytes bytes = Load(_folder + "/manifest.json");
    if (bytes.empty()) return;
    const JsValue parsed = JsParseString(std::string(bytes.begin(), bytes.end()));
    if (!parsed.IsObject()) return;
    const auto &o = parsed.GetJsObject();
    auto load = [&](const char *key, auto accept) {
        auto it = o.find(key); if (it == o.end() || !it->second.IsArray()) return;
        for (const auto &v : it->second.GetJsArray()) if (v.IsObject()) accept(v.GetJsObject());
    };
    load("artifacts", [&](JsObject a) {
        const std::string id = Text(a, "artifact_id");
        // File paths are reconstructed, never trusted from the manifest.
        if (id.empty() || id.find_first_not_of("0123456789-") != std::string::npos) return;
        a["path"] = JsValue(_folder + "/" + id + ".png"); _artifacts[id] = std::move(a);
    });
    load("jobs", [&](JsObject j) {
        const std::string id = Text(j, "job_id"); if (id.empty()) return;
        if (!Terminal(Text(j, "state"))) {
            j["state"] = JsValue("failed"); j["error"] = JsValue("interrupted: Vela restarted; not resubmitted");
        }
        _jobs[id] = {0, Text(j, "document_key"), Text(j, "owner"), Text(j, "fingerprint"), j, {}};
    });
    Save();
}
void Service::Register(Provider provider) { _providers[provider.model.connection] = std::move(provider); }
void Service::Bind(int d, const std::string &key) { _documents[d] = key; }
std::string Service::Key(int d) const { auto i = _documents.find(d); return i == _documents.end() ? "" : i->second; }
void Service::SetOwner(int d, std::string owner) { _owners[d] = std::move(owner); }
void Service::CancelOwner(int d, const std::string &owner) {
    std::vector<std::function<void()>> cancels;
    for (auto &[id,j] : _jobs) if (j.session == d && j.owner == owner && !Terminal(Text(j.value,"state"))) {
        j.value["state"] = JsValue("cancelled"); if (j.cancel) cancels.push_back(std::move(j.cancel));
    }
    Save(); for (const auto &cancel : cancels) cancel(); Notify(d); Pump();
}
void Service::Close(int d) {
    // Mark every job before cancelling tasks: cancellation can complete inline,
    // and must not start another queued request for a closing document.
    std::vector<std::function<void()>> cancels;
    for (auto &[id,j] : _jobs) if (j.session == d && !Terminal(Text(j.value,"state"))) {
        j.value["state"] = JsValue("cancelled"); if (j.cancel) cancels.push_back(std::move(j.cancel));
    }
    Save(); _documents.erase(d); _owners.erase(d);
    for (const auto &cancel : cancels) cancel();
    Pump();
}
JsArray Service::Models() const {
    JsArray a;
    const auto &settings = AppSettingsStore::Get().GetSettings();
    for (const auto &[id,p] : _providers) a.push_back(JsValue(JsObject{
        {"connection_id",JsValue(id)}, {"provider",JsValue(p.model.provider)}, {"model",JsValue(p.model.name)},
        {"host",JsValue(p.model.host)}, {"ready",JsValue(p.ready && p.ready())},
        {"enabled",JsValue(settings.imageGeneration)}, {"default",JsValue(id == settings.imageConnection)},
        {"max_references",JsValue(p.model.maxReferences)}, {"sizes",JsValue(Strings(p.model.sizes))},
        {"qualities",JsValue(Strings(p.model.qualities))}, {"backgrounds",JsValue(Strings(p.model.backgrounds))}}));
    return a;
}
Result Service::Submit(int d, const JsObject &input) {
    if (Key(d).empty() || _folder.empty()) return Error("Image service is unavailable for this document");
    const auto &s = AppSettingsStore::Get().GetSettings();
    if (!s.imageGeneration) return Error("Enable Image Generation in Settings first");
    const std::set<std::string> fields{"request_id","prompt","purpose","reference_ids","connection_id","model","size","quality","background","provider_options"};
    for (const auto &[k,v] : input) {
        if (!fields.count(k)) return Error("Unknown image option: " + k);
        if (k != "reference_ids" && k != "provider_options" && !v.IsString()) return Error(k + " must be a string");
    }
    if (auto it = input.find("provider_options"); it != input.end() &&
        (!it->second.IsObject() || !it->second.GetJsObject().empty())) return Error("This adapter has no provider-specific options");
    JsObject o = input;
    const std::string prompt = Text(o,"prompt"), request = Text(o,"request_id");
    if (prompt.empty() || prompt.size() > 32000 || prompt.find_first_not_of(" \t\r\n") == std::string::npos) return Error("Prompt must contain 1–32000 bytes of text");
    if (request.empty() || request.size() > 128) return Error("request_id must contain 1–128 bytes");
    const std::string connection = Text(o,"connection_id",s.imageConnection);
    auto pi = _providers.find(connection);
    if (pi == _providers.end()) return Error("Unknown connection; use list_image_models");
    const Model &m = pi->second.model;
    if (Text(o,"model",m.name) != m.name) return Error("Unsupported model; use list_image_models");
    o["connection_id"] = JsValue(connection); o["model"] = JsValue(m.name);
    for (const auto &[key,allowed] : std::vector<std::pair<std::string,std::vector<std::string>>>{
        {"size",m.sizes},{"quality",m.qualities},{"background",m.backgrounds}}) {
        const std::string value = Text(o,key,allowed.empty() ? "" : allowed.front());
        if (!Contains(allowed,value)) return Error("Unsupported " + key + "; use list_image_models");
        o[key] = JsValue(value);
    }
    if (Text(o,"quality") == "high" && !s.imageHighQuality) return Error("High quality is disabled in Image Generation Settings");
    if (!o.count("reference_ids")) o["reference_ids"] = JsValue(JsArray());
    if (!o["reference_ids"].IsArray() || o["reference_ids"].GetJsArray().size() > size_t(m.maxReferences)) return Error("Too many reference images for this model");
    size_t total = 0;
    for (const auto &v : o["reference_ids"].GetJsArray()) {
        if (!v.IsString() || !Artifact(d,v.GetString())) return Error("Reference must be an artifact in this document");
        Bytes b = Read(d,v.GetString()); if (b.empty()) return Error("Reference image is missing"); total += b.size();
    }
    if (total > 20*1024*1024) return Error("Reference images exceed 20 MB");
    const std::string owner = _owners[d], fingerprint = mcp::Compact(JsValue(o));
    for (const auto &[id,j] : _jobs) if (j.document == Key(d) && j.owner == owner && Text(j.value,"request_id") == request) {
        return j.fingerprint == fingerprint ? Result{j.value,{}} : Error("request_id conflicts with an earlier request");
    }
    size_t pending = 0;
    for (const auto &[id,j] : _jobs) if (j.document == Key(d) && !Terminal(Text(j.value,"state"))) ++pending;
    if (pending >= 8 || _requests >= unsigned(s.imageRequestLimit)) return Error("Image request limit reached");
    if (!pi->second.ready || !pi->second.ready()) return Error("Add this provider's API key in Image Generation Settings");
    const std::string id = Id();
    JsObject j = o;
    j["job_id"] = JsValue(id); j["state"] = JsValue(s.imageAsk ? "awaiting_approval" : "queued");
    j["document_key"] = JsValue(Key(d)); j["owner"] = JsValue(owner); j["fingerprint"] = JsValue(fingerprint);
    j["provider"] = JsValue(m.provider); j["host"] = JsValue(m.host);
    j["created"] = JsValue(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
    _jobs[id] = {d,Key(d),owner,fingerprint,j,{}}; ++_requests;
    if (!Save()) { _jobs.erase(id); return Error("Cannot persist image job; request was not sent"); }
    Notify(d); Pump(); return Job(d,id);
}
void Service::Notify(int d) { if (changed) changed(d); }
bool Service::Save() const {
    if (_folder.empty()) return false;
    JsArray jobs, artifacts;
    for (const auto &[id,j] : _jobs) jobs.push_back(JsValue(j.value));
    for (const auto &[id,a] : _artifacts) artifacts.push_back(JsValue(a));
    const std::string json = mcp::Compact(JsValue(JsObject{{"version",JsValue(1)},{"jobs",JsValue(jobs)},{"artifacts",JsValue(artifacts)}}));
    return Write(_folder + "/manifest.json",Bytes(json.begin(),json.end()));
}
Result Service::Job(int d, const std::string &id) const {
    auto it = _jobs.find(id);
    if (Key(d).empty() || it == _jobs.end() || it->second.document != Key(d)) return Error("No image job in this document has that ID");
    return {it->second.value,{}};
}
JsArray Service::Jobs(int d) const {
    JsArray a; for (const auto &[id,j] : _jobs) if (!Key(d).empty() && j.document == Key(d)) a.push_back(JsValue(j.value)); return a;
}
Result Service::Approve(int d, const std::string &id, bool allow) {
    if (!Job(d,id)) return Job(d,id);
    auto &j = _jobs.at(id);
    if (Text(j.value,"state") != "awaiting_approval") return Error("Job is no longer awaiting approval");
    if (!allow) return Cancel(d,id);
    if (!AppSettingsStore::Get().GetSettings().imageGeneration) return Error("Image Generation is disabled");
    j.value["state"] = JsValue("queued");
    if (!Save()) { j.value["state"] = JsValue("awaiting_approval"); return Error("Cannot persist approval"); }
    Pump(); Notify(d); return Job(d,id);
}
Result Service::Cancel(int d, const std::string &id) {
    if (!Job(d,id)) return Job(d,id);
    auto &j = _jobs.at(id);
    if (!Terminal(Text(j.value,"state"))) {
        j.value["state"] = JsValue("cancelled"); auto cancel = std::move(j.cancel);
        Save(); if (cancel) cancel(); Notify(d); Pump();
    }
    return Job(d,id);
}
void Service::Pump() {
    std::set<std::string> busy;
    for (const auto &[id,j] : _jobs) if (Text(j.value,"state") == "running") busy.insert(j.document);
    std::vector<std::string> starts;
    for (const auto &[id,j] : _jobs) if (Text(j.value,"state") == "queued" && !busy.count(j.document)) { busy.insert(j.document); starts.push_back(id); }
    for (const auto &id : starts) Start(id);
}
void Service::Start(const std::string &id) {
    auto &j = _jobs.at(id);
    if (Text(j.value,"state") != "queued") return;
    auto p = _providers.find(Text(j.value,"connection_id"));
    if (!AppSettingsStore::Get().GetSettings().imageGeneration || p == _providers.end() || !p->second.ready()) {
        j.value["state"] = JsValue("failed"); j.value["error"] = JsValue("Provider disabled or credentials unavailable"); Save(); Notify(j.session); Pump(); return;
    }
    Request r{id,j.document,j.owner,p->second.model,j.value,{}};
    for (const auto &v : j.value.at("reference_ids").GetJsArray()) r.references.push_back(Read(j.session,v.GetString()));
    j.value["state"] = JsValue("running");
    if (!Save()) { j.value["state"] = JsValue("failed"); j.value["error"] = JsValue("Cannot persist running job"); Notify(j.session); Pump(); return; }
    auto cancel = p->second.submit(std::move(r),[this,id](Output o) { Complete(id,std::move(o)); });
    if (Text(j.value,"state") == "running") j.cancel = std::move(cancel);
    Notify(j.session);
}
void Service::Complete(const std::string &id, Output o) {
    auto it = _jobs.find(id); if (it == _jobs.end() || Text(it->second.value,"state") != "running") return;
    auto &j = it->second; j.cancel = {};
    if (o.error.empty()) {
        JsObject source;
        for (const char *key : {"job_id","prompt","provider","connection_id","model","reference_ids",
                                "size","quality","background","purpose","created"}) {
            if (j.value.count(key)) source[key] = j.value.at(key);
        }
        source["provider_request_id"] = JsValue(o.requestId); source["usage"] = JsValue(o.usage);
        auto a = AddReference(j.session,o,source);
        if (a) { j.value["artifact_id"] = a.value.at("artifact_id"); j.value["state"] = JsValue("succeeded"); }
        else o.error = a.error;
    }
    if (!o.error.empty()) { j.value["state"] = JsValue("failed"); j.value["error"] = JsValue(o.error); }
    j.value["provider_request_id"] = JsValue(o.requestId); j.value["usage"] = JsValue(o.usage);
    Save(); Notify(j.session); Pump();
}
Result Service::AddReference(int d, Output o, const JsObject &source) {
    if (Key(d).empty() || o.png.empty() || o.png.size() > 32*1024*1024 || o.preview.empty() ||
        o.width <= 0 || o.height <= 0 || int64_t(o.width)*o.height > 16777216) return Error("Invalid or oversized image");
    const std::string id = Id(), path = _folder + "/" + id + ".png";
    if (!Write(path,o.png) || !Write(_folder + "/" + id + "-preview.png",o.preview)) {
        fs::remove(path); fs::remove(path + ".tmp"); fs::remove(_folder + "/" + id + "-preview.png.tmp"); return Error("Cannot save image artifact");
    }
    JsObject a = source;
    a["artifact_id"] = JsValue(id); a["document_key"] = JsValue(Key(d)); a["path"] = JsValue(path);
    a["width"] = JsValue(o.width); a["height"] = JsValue(o.height); a["mime_type"] = JsValue("image/png");
    a["color_space"] = JsValue("sRGB"); a["sha256"] = JsValue(o.sha256);
    if (!a.count("created")) a["created"] = JsValue(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
    _artifacts[id] = a;
    if (!Save()) { _artifacts.erase(id); fs::remove(path); fs::remove(_folder + "/" + id + "-preview.png"); return Error("Cannot persist image artifact"); }
    Notify(d); return {a,{}};
}
Result Service::Artifact(int d, const std::string &id) const {
    auto it = _artifacts.find(id);
    if (Key(d).empty() || it == _artifacts.end() || Text(it->second,"document_key") != Key(d)) return Error("No image artifact in this document has that ID");
    return {it->second,{}};
}
JsArray Service::Artifacts(int d) const {
    JsArray a; for (const auto &[id,v] : _artifacts) if (!Key(d).empty() && Text(v,"document_key") == Key(d)) a.push_back(JsValue(v)); return a;
}
Bytes Service::Read(int d, const std::string &id, bool preview) const {
    auto a = Artifact(d,id); if (!a) return {};
    return Load(_folder + "/" + id + (preview ? "-preview.png" : ".png"));
}
} // namespace vela::images
