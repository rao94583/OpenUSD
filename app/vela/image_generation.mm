#import "image_generation.h"
#import "image_provider.h"
#import "document.h"
#include "app/vela/core/actions.h"
#include "app/vela/core/app_settings.h"
#include "app/vela/core/document.h"
#include "app/vela/core/session.h"
#include "app/vela/core/mcp.h"
#include "pxr/base/js/json.h"
#include <map>
#include <algorithm>

using namespace vela::images;
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
bool UseDefaults = false;
std::map<int,std::string> Keys;
int Find(vela::Document &document) {
    for (const auto &entry : vela::Session::Get().GetDocuments()) if (entry.document.get() == &document) return entry.id;
    return 0;
}
std::string DocumentKey(const vela::SessionDocument &entry) {
    const std::string path = entry.document->GetStage()->GetRootLayer()->GetRealPath();
    NSMutableDictionary *associations = UseDefaults ? [NSMutableDictionary dictionary] :
        [[NSUserDefaults.standardUserDefaults dictionaryForKey:@"VelaImageDocuments"] mutableCopy] ?: [NSMutableDictionary dictionary];
    auto it = Keys.find(entry.id);
    NSString *key = it != Keys.end() ? @(it->second.c_str()) : associations[@(path.c_str())];
    if (!key.length) key = NSUUID.UUID.UUIDString;
    Keys[entry.id] = key.UTF8String;
    if (!path.empty() && !UseDefaults) {
        associations[@(path.c_str())] = key;
        [NSUserDefaults.standardUserDefaults setObject:associations forKey:@"VelaImageDocuments"];
    }
    return key.UTF8String;
}
NSTextField *Note(NSString *s) {
    NSTextField *f = [NSTextField wrappingLabelWithString:s]; f.preferredMaxLayoutWidth = 640; return f;
}
NSButton *Button(NSString *s, id target, SEL action) { return [NSButton buttonWithTitle:s target:target action:action]; }
void Alert(std::string error) {
    if (error.empty()) return;
    NSAlert *a = [NSAlert new]; a.messageText = @"Image Generation"; a.informativeText = @(error.c_str()); [a runModal];
}
NSStackView *Stack(NSArray<NSView *> *views) {
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 10; stack.edgeInsets = NSEdgeInsetsMake(16,16,16,16); return stack;
}
}
@interface VelaImageSettings : NSWindowController
@end
@implementation VelaImageSettings {
    NSButton *_enabled, *_ask, *_high;
    NSPopUpButton *_provider;
    NSSecureTextField *_key;
    NSTextField *_status, *_limit;
}
- (instancetype)init {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,560,380) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:YES];
    window.title = @"Image Generation Settings";
    if (!(self = [super initWithWindow:window])) return nil;
    _enabled = [NSButton checkboxWithTitle:@"Enable image generation" target:self action:@selector(save:)];
    _ask = [NSButton checkboxWithTitle:@"Ask before each request (including reference upload)" target:self action:@selector(save:)];
    _high = [NSButton checkboxWithTitle:@"Allow high quality" target:self action:@selector(save:)];
    _provider = [NSPopUpButton new]; [_provider addItemsWithTitles:@[@"OpenAI — GPT Image",@"Google — Gemini"]];
    _provider.target = self; _provider.action = @selector(provider:);
    _key = [NSSecureTextField textFieldWithString:@""]; _key.placeholderString = @"New API key (stored in Keychain)";
    [_key.widthAnchor constraintEqualToConstant:480].active = YES;
    _limit = [NSTextField textFieldWithString:@"20"]; _limit.target=self; _limit.action=@selector(save:);
    [_limit.widthAnchor constraintEqualToConstant:60].active=YES;
    _status = Note(@"");
    NSStackView *buttons = [NSStackView stackViewWithViews:@[Button(@"Save Key",self,@selector(saveKey:)),Button(@"Remove Key",self,@selector(removeKey:))]];
    window.contentView = Stack(@[_enabled,_provider,_status,
        Note(@"Calls use provider API billing. ChatGPT-plan image generation is not supported. Only your prompt and chosen references are uploaded."),
        _key,buttons,_ask,_high,Note(@"Maximum requests per Vela launch (1–100):"),_limit]);
    [window setContentSize:window.contentView.fittingSize]; [window center]; [self refresh]; return self;
}
- (NSString *)connection { return _provider.indexOfSelectedItem == 1 ? @"gemini" : @"openai"; }
- (void)refresh {
    const auto &s = vela::AppSettingsStore::Get().GetSettings();
    _enabled.state=s.imageGeneration; _ask.state=s.imageAsk; _high.state=s.imageHighQuality;
    [_provider selectItemAtIndex:s.imageConnection == "gemini" ? 1 : 0]; _limit.integerValue=s.imageRequestLimit;
    _status.stringValue = VelaImageKey([self connection]).length ? @"API key saved in Keychain." : @"No API key saved.";
}
- (void)save:(id)sender {
    auto s = vela::AppSettingsStore::Get().GetSettings(); s.imageGeneration=_enabled.state; s.imageAsk=_ask.state; s.imageHighQuality=_high.state;
    s.imageConnection = [self connection].UTF8String; s.imageRequestLimit=int(MAX(1,MIN(100,_limit.integerValue)));
    vela::AppSettingsStore::Get().SetSettings(s); [self refresh];
}
- (void)provider:(id)sender { _key.stringValue=@""; [self save:sender]; }
- (void)saveKey:(id)sender {
    if (VelaSetImageKey([self connection],_key.stringValue)) _key.stringValue=@"";
    else Alert("Cannot save the API key in Keychain"); [self refresh];
}
- (void)removeKey:(id)sender { if (!VelaSetImageKey([self connection],@"")) Alert("Cannot remove API key"); [self refresh]; }
@end
void VelaShowImageSettings() { static VelaImageSettings *settings = [VelaImageSettings new]; [settings refresh]; [settings showWindow:nil]; }

@interface VelaImagesWindow : NSWindowController
@property(nonatomic) int documentId;
- (void)refresh;
@end
namespace {
NSMutableDictionary<NSNumber *,VelaImagesWindow *> *Windows() { static auto *windows = [NSMutableDictionary new]; return windows; }
void Show(int d) {
    if (!vela::Session::Get().Find(d)) return;
    VelaImagesWindow *w = Windows()[@(d)];
    if (!w) { w=[VelaImagesWindow new]; w.documentId=d; Windows()[@(d)]=w; }
    [w refresh]; [w showWindow:nil];
}
}
@implementation VelaImagesWindow {
    NSPopUpButton *_jobs;
    NSTextView *_details;
    NSImageView *_preview;
    NSStackView *_references;
    NSButton *_approve, *_cancel, *_save, *_continue;
    std::vector<std::string> _ids;
    std::string _artifact;
    NSTimer *_timer;
    NSWindow *_full;
}
- (instancetype)init {
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,740,670) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:YES];
    w.title = @"Generated Images";
    if (!(self=[super initWithWindow:w])) return nil;
    _jobs=[NSPopUpButton new]; _jobs.target=self; _jobs.action=@selector(selected:);
    [_jobs.widthAnchor constraintEqualToConstant:690].active=YES;
    _details=[NSTextView new]; _details.editable=NO; _details.verticallyResizable=YES; _details.autoresizingMask=NSViewWidthSizable; _details.font=[NSFont systemFontOfSize:13];
    NSScrollView *scroll=[NSScrollView new]; scroll.documentView=_details; scroll.hasVerticalScroller=YES;
    [scroll.widthAnchor constraintEqualToConstant:690].active=YES; [scroll.heightAnchor constraintEqualToConstant:200].active=YES;
    _references=[NSStackView stackViewWithViews:@[]]; _references.spacing=8;
    _preview=[NSImageView new]; _preview.imageScaling=NSImageScaleProportionallyUpOrDown;
    [_preview.widthAnchor constraintEqualToConstant:690].active=YES; [_preview.heightAnchor constraintEqualToConstant:220].active=YES;
    _approve=Button(@"Approve Request",self,@selector(approve:)); _cancel=Button(@"Cancel / Deny",self,@selector(cancel:));
    _save=Button(@"Save As…",self,@selector(export:)); _continue=Button(@"Continue with Claude",self,@selector(continue:));
    NSStackView *buttons=[NSStackView stackViewWithViews:@[_approve,_cancel,_save,_continue]];
    w.contentView=Stack(@[_jobs,scroll,_references,_preview,buttons,
        [NSStackView stackViewWithViews:@[Button(@"Import Reference…",self,@selector(import:)),Button(@"Reveal",self,@selector(reveal:)),Button(@"Full Size",self,@selector(fullSize:)),Button(@"Settings…",self,@selector(settings:))]],
        Note(@"Generated concepts are not scene renders. Save a texture into your project before applying it. Cancelling may not prevent provider charges.")]);
    [w setContentSize:w.contentView.fittingSize]; [w center];
    __weak VelaImagesWindow *weakSelf=self;
    _timer=[NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *) { if (weakSelf.window.isVisible) [weakSelf refresh]; }];
    return self;
}
- (void)refresh {
    std::string selected;
    NSInteger index=_jobs.indexOfSelectedItem; if (index>=0 && size_t(index)<_ids.size()) selected=_ids[index];
    const auto previousIds=_ids;
    std::string newest; double newestTime=-1;
    _ids.clear(); [_jobs removeAllItems];
    for (const auto &v : Service::Get().Jobs(_documentId)) {
        const auto &j=v.GetJsObject(); const std::string id=Text(j,"job_id"); _ids.push_back(id);
        const double created=j.count("created") && j.at("created").IsReal() ? j.at("created").GetReal() : 0;
        if (std::find(previousIds.begin(),previousIds.end(),id)==previousIds.end() && created>=newestTime) {
            newest=id; newestTime=created;
        }
        std::string label=Text(j,"state")+" · "+Text(j,"provider")+" · "+Text(j,"prompt").substr(0,65);
        [_jobs addItemWithTitle:@(label.c_str())];
    }
    for (const auto &v : Service::Get().Artifacts(_documentId)) {
        const auto &a=v.GetJsObject(); if (a.count("job_id")) continue;
        _ids.push_back(Text(a,"artifact_id")); [_jobs addItemWithTitle:@(("Reference · "+Text(a,"artifact_id")).c_str())];
    }
    if (!newest.empty()) selected=newest;
    auto it=std::find(_ids.begin(),_ids.end(),selected); if (it!=_ids.end()) [_jobs selectItemAtIndex:it-_ids.begin()];
    [self selected:nil];
}
- (std::string)selectedId { NSInteger i=_jobs.indexOfSelectedItem; return i>=0 && size_t(i)<_ids.size() ? _ids[i] : ""; }
- (void)selected:(id)sender {
    auto result=Service::Get().Job(_documentId,[self selectedId]);
    const std::string state=Text(result.value,"state");
    const JsObject job=result.value;
    _approve.enabled=state=="awaiting_approval"; _cancel.enabled=state=="awaiting_approval"||state=="queued"||state=="running";
    _artifact=Text(result.value,"artifact_id");
    if (!result) { result=Service::Get().Artifact(_documentId,[self selectedId]); _artifact=Text(result.value,"artifact_id"); }
    if (result) {
        const std::string description =
            "Status: " + (state.empty() ? "Reference image" : state) +
            "\nProvider: " + Text(job,"provider", "local") + " · " + Text(job,"model") +
            "\nDestination: " + Text(job,"host","Local library") +
            "\nPrompt: " + Text(job,"prompt", "Imported or captured reference") +
            "\nSize: " + Text(job,"size","original") + " · Quality: " + Text(job,"quality","original") +
            "\nBackground: " + Text(job,"background","original") +
            "\nReferences: " + std::to_string(job.count("reference_ids") ? job.at("reference_ids").GetJsArray().size() : 0) +
            "\n" + (state=="awaiting_approval" ? "Approve to send this prompt and references to the provider (API billing applies)." : Text(job,"error")) +
            (_artifact.empty() ? "" : "\nArtifact ID: " + _artifact);
        _details.string=@(description.c_str());
    } else _details.string=@"No images yet. Use Generate Image… or ask Claude to generate one.";
    std::string previewArtifact=_artifact;
    if (previewArtifact.empty() && job.count("reference_ids") && !job.at("reference_ids").GetJsArray().empty())
        previewArtifact=job.at("reference_ids").GetJsArray()[0].GetString();
    Bytes b=Service::Get().Read(_documentId,previewArtifact,true);
    _preview.image=b.empty()?nil:[[NSImage alloc] initWithData:[NSData dataWithBytes:b.data() length:b.size()]];
    for (NSView *view in [_references.arrangedSubviews copy]) { [_references removeArrangedSubview:view]; [view removeFromSuperview]; }
    if (job.count("reference_ids")) for (const auto &v : job.at("reference_ids").GetJsArray()) {
        Bytes bytes=Service::Get().Read(_documentId,v.GetString(),true);
        NSImageView *reference=[NSImageView new]; reference.imageScaling=NSImageScaleProportionallyUpOrDown;
        reference.image=[[NSImage alloc] initWithData:[NSData dataWithBytes:bytes.data() length:bytes.size()]];
        reference.toolTip=@(v.GetString().c_str());
        [reference.widthAnchor constraintEqualToConstant:96].active=YES;
        [reference.heightAnchor constraintEqualToConstant:70].active=YES;
        [_references addArrangedSubview:reference];
    }
    _references.hidden=_references.arrangedSubviews.count==0;
    _save.enabled=_continue.enabled=!_artifact.empty();
}
- (void)approve:(id)sender { Alert(Service::Get().Approve(_documentId,[self selectedId],true).error); [self refresh]; }
- (void)cancel:(id)sender { Alert(Service::Get().Cancel(_documentId,[self selectedId]).error); [self refresh]; }
- (void)settings:(id)sender { VelaShowImageSettings(); }
- (void)export:(id)sender {
    auto a=Service::Get().Artifact(_documentId,_artifact); if (!a) return;
    NSSavePanel *panel=[NSSavePanel savePanel]; panel.nameFieldStringValue=@"generated-image.png";
    panel.allowedFileTypes=@[@"png"];
    const std::string path=Text(a.value,"path");
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        if (response!=NSModalResponseOK) return;
        NSError *error=nil; NSData *bytes=[NSData dataWithContentsOfFile:@(path.c_str())];
        if (!bytes || ![bytes writeToURL:panel.URL options:NSDataWritingAtomic error:&error]) Alert("Cannot export image");
    }];
}
- (void)fullSize:(id)sender {
    auto a=Service::Get().Artifact(_documentId,_artifact); if (!a) return;
    NSImage *image=[[NSImage alloc] initWithContentsOfFile:@(Text(a.value,"path").c_str())];
    if (!image) return;
    if (!_full) {
        _full=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,800,650) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:YES];
        _full.releasedWhenClosed=NO; _full.title=@"Generated Image — Full Size"; [_full center];
    }
    NSImageView *view=[[NSImageView alloc] initWithFrame:NSMakeRect(0,0,image.size.width,image.size.height)];
    view.image=image; view.imageScaling=NSImageScaleNone;
    NSScrollView *scroll=[NSScrollView new]; scroll.hasHorizontalScroller=YES; scroll.hasVerticalScroller=YES; scroll.documentView=view;
    _full.contentView=scroll; [_full makeKeyAndOrderFront:nil];
}
- (void)close { [_full close]; [super close]; }
- (void)reveal:(id)sender {
    auto a=Service::Get().Artifact(_documentId,_artifact); if (a) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:@(Text(a.value,"path").c_str())]]];
}
- (void)continue:(id)sender {
    const auto *entry=vela::Session::Get().Find(_documentId); if (!entry || _artifact.empty()) return;
    auto r=vela::ActionRegistry::Get().Run(*entry->document,"ask_claude",JsObject{{"prompt",JsValue("Show and discuss generated image artifact " + _artifact + ". Use get_generated_image to inspect it.")}});
    if (!r.ok) Alert(r.error);
}
- (void)import:(id)sender {
    NSOpenPanel *panel=[NSOpenPanel openPanel]; panel.allowedFileTypes=@[@"png",@"jpg",@"jpeg",@"webp"];
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        if (response!=NSModalResponseOK) return;
        NSNumber *size=nil; [panel.URL getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        if (size.longLongValue>32*1024*1024) { Alert("Reference exceeds 32 MB"); return; }
        auto output=VelaNormalizeImage([NSData dataWithContentsOfURL:panel.URL]);
        if (!output.error.empty()) { Alert(output.error); return; }
        Alert(Service::Get().AddReference(self.documentId,std::move(output),JsObject{{"source",JsValue("user_import")}}).error); [self refresh];
    }];
}
@end
void VelaInitializeImages(bool useDefaults) {
    UseDefaults=useDefaults;
    NSString *root = useDefaults ? [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString] :
        [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Vela/Generated Images"];
    if (const char *test = getenv("VELA_IMAGE_LIBRARY")) root=@(test);
    Service::Get().Configure(root.UTF8String); VelaRegisterImageProviders();
    vela::Session::Get().removed=[](int d) {
        [Windows()[@(d)] close]; [Windows() removeObjectForKey:@(d)]; Keys.erase(d);
    };
    vela::Session::Get().added=[](const vela::SessionDocument &entry) { Service::Get().Bind(entry.id,DocumentKey(entry)); };
    Service::Get().changed=[](int d) {
        // Schedule, rather than re-enter submission while its state is changing.
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!vela::Session::Get().Find(d)) return;
            Show(d);
        });
    };
    // Fixture providers are available only with --defaultsettings. They never
    // use Keychain or HTTP, and leave the user's provider configuration alone.
    if (useDefaults) if (const char *fixture=getenv("VELA_IMAGE_FIXTURE")) {
        const std::string path=fixture;
        for (const char *connection : {"openai","gemini"}) {
            Provider p; p.model={connection,connection,"fixture-model","fixture.invalid",3,{"1024x1024"},{"medium"},{"opaque"}};
            p.ready=[] { return true; };
            p.submit=[path](Request r,std::function<void(Output)> done) {
                auto cancelled=std::make_shared<bool>(false);
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW,300*NSEC_PER_MSEC),dispatch_get_main_queue(),^{
                    if (!*cancelled) done(VelaNormalizeImage([NSData dataWithContentsOfFile:@(path.c_str())]));
                });
                return std::function<void()>([cancelled] { *cancelled=true; });
            };
            Service::Get().Register(std::move(p));
        }
        auto settings=vela::AppSettingsStore::Get().GetSettings(); settings.imageGeneration=true;
        settings.imageAsk=getenv("VELA_IMAGE_TEST_AUTO")==nullptr;
        vela::AppSettingsStore::Get().SetSettings(settings);
    }
    Service::Get().normalize=[](const Bytes &b) { return VelaNormalizeImage([NSData dataWithBytes:b.data() length:b.size()]); };
}
void VelaRegisterImageActions() {
    vela::Action settings; settings.name="image_generation_settings"; settings.title="Image Generation Settings…";
    settings.description="Configure image providers and credentials."; settings.menu="Window";
    settings.run=[](vela::Document &,const JsObject &) { VelaShowImageSettings(); return vela::ActionResult{}; };
    vela::ActionRegistry::Get().Register(std::move(settings));
    vela::Action library; library.name="generated_images"; library.title="Generated Images…"; library.menu="Window";
    library.description="Preview, approve, cancel and save this document's generated images.";
    library.run=[](vela::Document &d,const JsObject &) { Show(Find(d)); return vela::ActionResult{}; };
    vela::ActionRegistry::Get().Register(std::move(library));
    vela::Action generate; generate.name="generate_image"; generate.title="Generate Image…"; generate.menu="Window";
    generate.description="Generate a concept image or texture with the configured image provider.";
    generate.parameters=vela::ParameterSchema(R"({"properties":{"prompt":{"type":"string"},"reference_id":{"type":"string"}},"required":["prompt"]})");
    generate.form=[](const vela::Document &) {
        vela::FormField prompt; prompt.key="prompt"; prompt.label="Image prompt"; prompt.value=JsValue("");
        vela::FormField reference; reference.key="reference_id"; reference.label="Reference artifact ID (optional)"; reference.value=JsValue("");
        return std::vector<vela::FormField>{prompt,reference};
    };
    generate.run=[](vela::Document &d,const JsObject &params) {
        const int id=Find(d); const std::string previous=Service::Get().Owner(id); Service::Get().SetOwner(id,"native");
        JsArray refs; if (!Text(params,"reference_id").empty()) refs.push_back(JsValue(Text(params,"reference_id")));
        auto r=Service::Get().Submit(id,JsObject{{"request_id",JsValue(NSUUID.UUID.UUIDString.UTF8String)},{"prompt",params.at("prompt")},{"reference_ids",JsValue(refs)}});
        Service::Get().SetOwner(id,previous);
        return r ? vela::ActionResult{true,JsValue(r.value),{}} : vela::ActionResult::Error(r.error);
    };
    vela::ActionRegistry::Get().Register(std::move(generate));
}
