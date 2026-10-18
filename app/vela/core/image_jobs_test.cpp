#include "image_jobs.h"
#include "app_settings.h"
#include "test_support.h"
#include <filesystem>
#include <unistd.h>
using namespace vela;
using namespace vela::images;
int main() {
    const auto folder=std::filesystem::temp_directory_path()/("vela-images-"+std::to_string(getpid()));
    std::filesystem::remove_all(folder);
    Service s; s.Configure(folder.string()); s.Bind(1,"document-a"); s.Bind(2,"document-b");
    int calls=0,cancels=0; std::vector<std::function<void(Output)>> completions;
    Provider p; p.model={"fake","fake","model","fake.invalid",1,{"1024x1024"},{"medium"},{"opaque"}};
    p.ready=[] { return true; };
    p.submit=[&](Request r,std::function<void(Output)> done) {
        ++calls; CHECK(r.document=="document-a"); completions.push_back(done);
        return std::function<void()>([&] { ++cancels; });
    };
    s.Register(p);
    JsObject input{{"request_id",JsValue("one")},{"prompt",JsValue("fabric")}};
    CHECK(!s.Submit(1,input));
    auto settings=AppSettings(); settings.imageGeneration=true; settings.imageConnection="fake";
    AppSettingsStore::Get().SetSettings(settings);
    auto first=s.Submit(1,input); CHECK(first); CHECK(Text(first.value,"state")=="awaiting_approval"); CHECK(calls==0);
    const std::string id=Text(first.value,"job_id");
    CHECK(!s.Job(2,id)); CHECK(!s.Approve(2,id,true)); CHECK(!s.Cancel(2,id));
    CHECK(Text(s.Submit(1,input).value,"job_id")==id);
    auto conflict=input; conflict["prompt"]=JsValue("different"); CHECK(!s.Submit(1,conflict));
    auto bad=input; bad["approved"]=JsValue(true); CHECK(!s.Submit(1,bad));
    bad=input; bad["quality"]=JsValue("high"); CHECK(!s.Submit(1,bad));
    CHECK(s.Approve(1,id,true)); CHECK(calls==1);
    CHECK(s.Cancel(1,id)); CHECK(cancels==1);
    Output image; image.png={1,2,3}; image.preview={4,5}; image.width=image.height=64;
    completions[0](image); CHECK(Text(s.Job(1,id).value,"state")=="cancelled"); CHECK(s.Artifacts(1).empty());
    input["request_id"]=JsValue("two"); auto second=s.Submit(1,input);
    CHECK(s.Approve(1,Text(second.value,"job_id"),true)); CHECK(calls==2);
    input["request_id"]=JsValue("three"); auto third=s.Submit(1,input); CHECK(s.Approve(1,Text(third.value,"job_id"),true)); CHECK(calls==2);
    completions[1](image); CHECK(calls==3);
    auto result=s.Job(1,Text(second.value,"job_id")); CHECK(Text(result.value,"state")=="succeeded");
    const std::string artifact=Text(result.value,"artifact_id"); CHECK(s.Read(1,artifact)==image.png); CHECK(s.Read(2,artifact).empty());
    CHECK(!s.Artifact(1,"../../manifest"));
    // Cross-provider editing uses the artifact bytes, not a provider file ID.
    Provider other=p; other.model.connection="other"; other.model.provider="second";
    other.submit=[&](Request r,std::function<void(Output)> done) {
        CHECK(r.references.size()==1 && r.references[0]==image.png); done(image); return std::function<void()>();
    }; s.Register(other);
    JsObject edit{{"request_id",JsValue("edit")},{"prompt",JsValue("snow")},{"connection_id",JsValue("other")},{"reference_ids",JsValue(JsArray{JsValue(artifact)})}};
    auto editing=s.Submit(1,edit); CHECK(editing);
    s.Cancel(1,Text(third.value,"job_id")); CHECK(s.Approve(1,Text(editing.value,"job_id"),true));
    CHECK(Text(s.Job(1,Text(editing.value,"job_id")).value,"state")=="succeeded");
    // Reopen: completed assets survive, unfinished work never resubmits.
    input["request_id"]=JsValue("pending"); CHECK(s.Submit(1,input));
    Service restored; restored.Configure(folder.string()); restored.Bind(4,"document-a");
    CHECK(restored.Read(4,artifact)==image.png);
    for (const auto &v:restored.Jobs(4)) CHECK(Text(v.GetJsObject(),"state")!="awaiting_approval");
    // Closing must cancel queued jobs without ever starting them.
    input["request_id"]=JsValue("close-running"); auto running=s.Submit(1,input);
    s.Approve(1,Text(running.value,"job_id"),true);
    input["request_id"]=JsValue("close-queued"); auto queued=s.Submit(1,input);
    s.Approve(1,Text(queued.value,"job_id"),true);
    const int beforeClose=calls;
    s.Close(1); CHECK(calls==beforeClose); CHECK(!s.Job(1,id));
    AppSettingsStore::Get().Reset(); std::filesystem::remove_all(folder);
    return test::Finish("image jobs");
}
