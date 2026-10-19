#import "packages_ui.h"

#import <AppKit/AppKit.h>

#include "app/vela/core/actions.h"
#include "app/vela/core/claude_process.h"
#include "app/vela/core/log.h"
#include "app/vela/core/mcp.h"
#include "app/vela/core/packages.h"
#include "app/vela/core/plugins.h"
#include "app/vela/core/python.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

std::string Text(NSString *string) { return string.UTF8String ?: ""; }
NSString *String(const std::string &text) { return @(text.c_str()) ?: @""; }

NSString *Runtime() {
    return [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"Runtime"];
}

std::string Base() { return vela::packages::BaseFolder(vela::PythonVersion()); }
std::string Site() { return vela::packages::SitePackages(Base(), vela::PythonVersion()); }

NSString *Join(const std::vector<std::string> &items) {
    NSMutableArray *parts = [NSMutableArray array];
    for (const std::string &item : items) [parts addObject:String(item)];
    return [parts componentsJoinedByString:@", "];
}

// Set while resetting, which the user has agreed to already.
bool alreadyConfirmed = false;

// The user agrees, unless a test has said so already.
BOOL Confirm(NSString *requester, const std::vector<std::string> &requirements) {
    if (alreadyConfirmed) return YES;
    if (const char *approve = getenv("VELA_PACKAGES_AUTO_APPROVE"); approve && *approve == '1') {
        return YES;
    }
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"Install Python packages?";
    alert.informativeText = [NSString stringWithFormat:
        @"%@ needs %@.\n\nVela downloads them with pip from the Python Package Index (or the "
        @"index your pip configuration names) and installs them for your user account, in "
        @"%@. Packages run with your permissions, as plugins do: install only what you "
        @"trust.", requester, Join(requirements),
        [String(Base()) stringByAbbreviatingWithTildeInPath]];
    [alert addButtonWithTitle:@"Install"];
    [alert addButtonWithTitle:@"Cancel"];
    [NSApp activateIgnoringOtherApps:YES];
    return [alert runModal] == NSAlertFirstButtonReturn;
}

// pip's output, shared with the reader thread.
struct Progress {
    std::mutex mutex;
    std::vector<std::string> lines;
    bool done = false;
    int status = -1;
    std::string errors;
};

} // namespace

// A small window with pip's latest line and Cancel, run as a modal
// session while pip works.
@interface VelaPackageProgress : NSObject
@property (nonatomic) BOOL cancelled;
- (instancetype)initWithTitle:(NSString *)title;
- (void)show:(NSString *)line;
- (void)end;
@end

@implementation VelaPackageProgress {
    NSPanel *_panel;
    NSTextField *_line;
    NSProgressIndicator *_bar;
    NSModalSession _session;
}

- (instancetype)initWithTitle:(NSString *)title {
    if (!(self = [super init])) return nil;
    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 460, 110)
                                        styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered defer:NO];
    _panel.title = @"Installing Python Packages";
    _panel.releasedWhenClosed = NO;
    NSTextField *heading = [NSTextField labelWithString:title];
    heading.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    heading.lineBreakMode = NSLineBreakByTruncatingTail;
    _line = [NSTextField labelWithString:@"Starting pip…"];
    _line.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize
                                             weight:NSFontWeightRegular];
    _line.textColor = NSColor.secondaryLabelColor;
    _line.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _bar = [NSProgressIndicator new];
    _bar.indeterminate = YES;
    [_bar startAnimation:nil];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    cancel.keyEquivalent = @"\e";
    NSView *content = _panel.contentView;
    for (NSView *view in @[heading, _line, _bar, cancel]) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [content addSubview:view];
    }
    [NSLayoutConstraint activateConstraints:@[
        [heading.topAnchor constraintEqualToAnchor:content.topAnchor constant:14],
        [heading.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:18],
        [heading.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-18],
        [_bar.topAnchor constraintEqualToAnchor:heading.bottomAnchor constant:8],
        [_bar.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [_bar.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [_line.topAnchor constraintEqualToAnchor:_bar.bottomAnchor constant:6],
        [_line.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [_line.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [cancel.topAnchor constraintEqualToAnchor:_line.bottomAnchor constant:8],
        [cancel.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [cancel.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-12],
    ]];
    [_panel center];
    _session = [NSApp beginModalSessionForWindow:_panel];
    return self;
}

- (void)cancel:(id)sender { _cancelled = YES; }

- (void)show:(NSString *)line {
    if (line.length) _line.stringValue = line;
    [NSApp runModalSession:_session];
}

- (void)end {
    [NSApp endModalSession:_session];
    [_panel orderOut:nil];
}

@end

BOOL VelaInstallPythonPackages(NSArray<NSString *> *requested, NSString *requester,
                               NSString **message, NSString **logTail) {
    auto fail = [&](NSString *text) {
        if (message) *message = text;
        return NO;
    };
    if (!vela::IsPythonInitialized()) return fail(@"Python is not available");
    std::vector<std::string> fresh;
    for (NSString *requirement in requested) {
        const std::string text = Text([requirement stringByTrimmingCharactersInSet:
            NSCharacterSet.whitespaceAndNewlineCharacterSet]);
        const std::string problem = vela::packages::CheckRequirement(text);
        if (!problem.empty()) return fail(String(problem));
        fresh.push_back(text);
    }
    if (fresh.empty()) return fail(@"No packages to install");
    if (!Confirm(requester, fresh)) return fail(@"The user declined to install the packages.");

    // Everything the manifest asks for, with the new requirements: pip
    // resolves them together, keeping the bundle's own packages.
    const std::string base = Base();
    [NSFileManager.defaultManager createDirectoryAtPath:String(base)
                            withIntermediateDirectories:YES attributes:nil error:nil];
    const std::string bundled = Text([Runtime() stringByAppendingPathComponent:@"site-packages"]);
    const std::string constraintsPath = base + "/constraints.txt";
    {
        std::ofstream constraints(constraintsPath);
        for (const std::string &line : vela::packages::Constraints(bundled)) {
            constraints << line << "\n";
        }
    }
    std::vector<std::string> all;
    std::set<std::string> names;
    for (const std::string &requirement : fresh) {
        all.push_back(requirement);
        names.insert(vela::packages::RequirementName(requirement));
    }
    // Plugins' requirements always take part, so a request that conflicts
    // with one fails rather than breaking the plugin; a new request replaces
    // an earlier request for the same package.
    const vela::packages::Manifest manifest = vela::packages::ReadManifest(base);
    for (const auto &[plugin, list] : manifest.plugins) {
        all.insert(all.end(), list.begin(), list.end());
    }
    for (const std::string &requirement : manifest.requested) {
        if (!names.count(vela::packages::RequirementName(requirement))) all.push_back(requirement);
    }
    std::sort(all.begin() + fresh.size(), all.end());
    all.erase(std::unique(all.begin() + fresh.size(), all.end()), all.end());
    const char *extra = getenv("VELA_PIP_ARGS");
    const vela::packages::PipCommand command = vela::packages::InstallCommand(
        Text([Runtime() stringByAppendingPathComponent:@"python/bin/python3"]), base, bundled,
        constraintsPath, all, vela::packages::SplitArguments(extra ? extra : ""));
    std::string commandLine;
    for (const std::string &arg : command.args) commandLine += arg + " ";
    vela::Log::Get().Add(vela::LogEntry::Level::Info, "Packages", "Running " + commandLine);

    auto progress = std::make_shared<Progress>();
    vela::LineProcess pip;
    vela::LineProcess::Callbacks callbacks;
    callbacks.line = [progress](const std::string &line) {
        std::lock_guard<std::mutex> lock(progress->mutex);
        progress->lines.push_back(line);
    };
    callbacks.exited = [progress](int status, const std::string &errors) {
        std::lock_guard<std::mutex> lock(progress->mutex);
        progress->done = true;
        progress->status = status;
        progress->errors = errors;
    };
    std::string error;
    if (!pip.Start(command.args, base, command.environment, callbacks, &error)) {
        return fail(String("pip didn't start: " + error));
    }
    VelaPackageProgress *window = [[VelaPackageProgress alloc] initWithTitle:
        [NSString stringWithFormat:@"Installing %@", Join(fresh)]];
    size_t shown = 0;
    std::vector<std::string> output;
    bool done = false;
    int status = -1;
    std::string errors;
    while (!done) {
        NSString *latest = nil;
        {
            std::lock_guard<std::mutex> lock(progress->mutex);
            for (; shown < progress->lines.size(); ++shown) {
                output.push_back(progress->lines[shown]);
                latest = String(progress->lines[shown]);
            }
            done = progress->done;
            status = progress->status;
            errors = progress->errors;
        }
        [window show:latest];
        if (window.cancelled) {
            pip.Terminate();
            [window end];
            vela::Log::Get().Add(vela::LogEntry::Level::Warning, "Packages", "Installing cancelled");
            return fail(@"Installing was cancelled.");
        }
        if (!done) [NSThread sleepForTimeInterval:0.05];
    }
    [window end];

    std::string tail;
    for (size_t i = output.size() > 30 ? output.size() - 30 : 0; i < output.size(); ++i) {
        tail += output[i] + "\n";
    }
    if (!errors.empty()) tail += errors;
    if (logTail) *logTail = String(tail);
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    vela::Log::Get().Add(ok ? vela::LogEntry::Level::Info : vela::LogEntry::Level::Error,
                         "Packages", tail.empty() ? (ok ? "Installed" : "pip failed") : tail);
    if (!ok) return fail(String("pip couldn't install " + Text(Join(fresh)) + ":\n" + tail));

    // The new packages on Python's path.
    std::string result;
    vela::CallPython("_refresh_packages",
                     vela::mcp::Compact(JsValue(JsArray{JsValue(Site())})), &result, &error);
    return YES;
}

namespace {

// Installs what the waiting plugins need, then loads them.
vela::ActionResult InstallForPlugins() {
    const vela::PluginReport &report = vela::CurrentPlugins();
    const std::vector<std::string> missing = report.Missing();
    if (missing.empty()) return vela::ActionResult{true, JsValue("No plugin is waiting for packages"), ""};
    NSMutableArray *requirements = [NSMutableArray array];
    for (const std::string &requirement : missing) [requirements addObject:String(requirement)];
    std::vector<std::string> plugins;
    for (const vela::PluginWaiting &waiting : report.waiting) plugins.push_back(waiting.plugin);
    NSString *requester = plugins.size() == 1
        ? [NSString stringWithFormat:@"The plugin %@", String(plugins[0])]
        : [NSString stringWithFormat:@"The plugins %@", Join(plugins)];
    NSString *message = nil, *log = nil;
    if (!VelaInstallPythonPackages(requirements, requester, &message, &log)) {
        return vela::ActionResult::Error(Text(message));
    }
    return vela::ActionResult{true, JsValue(vela::LoadPlugins(false).ToJson()), ""};
}

} // namespace

void VelaRegisterPackageActions(void) {
    vela::Action install;
    install.name = "install_python_packages";
    install.title = "Install Python Packages…";
    install.description =
        "Install Python packages (PEP 508 requirements such as 'numpy>=2') from the package "
        "index into Vela's packages folder, for scripts and plugins. Vela asks the user first "
        "and resolves them together with every package already asked for. Plugins should "
        "declare theirs in plugInfo.json (\"PythonRequires\") instead.";
    install.parameters = vela::ParameterSchema(R"({
        "properties": {"packages": {"type": "array", "items": {"type": "string"},
                                    "description": "Requirements, e.g. [\"numpy>=2\"]"}},
        "required": ["packages"]})");
    install.asksUser = true;
    install.run = [](vela::Document &, const JsObject &params) {
        NSMutableArray *requirements = [NSMutableArray array];
        for (const JsValue &item : params.at("packages").GetJsArray()) {
            if (item.IsString()) [requirements addObject:String(item.GetString())];
        }
        NSString *message = nil, *log = nil;
        if (!VelaInstallPythonPackages(requirements, @"A script", &message, &log)) {
            return vela::ActionResult::Error(Text(message));
        }
        // Remembered, so resetting or a new Python reinstalls them.
        const std::string base = Base();
        vela::packages::Manifest manifest = vela::packages::ReadManifest(base);
        for (NSString *requirement in requirements) {
            const std::string text = Text(requirement);
            const std::string name = vela::packages::RequirementName(text);
            std::erase_if(manifest.requested, [&](const std::string &r) {
                return vela::packages::RequirementName(r) == name;
            });
            manifest.requested.push_back(text);
        }
        vela::packages::WriteManifest(base, manifest);
        JsObject result{{"installed", JsValue(Text([requirements componentsJoinedByString:@", "]))},
                        {"log", JsValue(Text(log ?: @""))}};
        // Plugins that were waiting may load now.
        if (!vela::CurrentPlugins().waiting.empty()) {
            result["plugins"] = JsValue(vela::LoadPlugins(false).ToJson());
        }
        return vela::ActionResult{true, JsValue(result), ""};
    };
    vela::ActionRegistry::Get().Register(std::move(install));

    vela::Action forPlugins;
    forPlugins.name = "install_plugin_packages";
    forPlugins.title = "Install Packages for Plugins…";
    forPlugins.description = "Install the Python packages that waiting plugins declare "
                             "(PythonRequires), after asking the user, then load those plugins.";
    forPlugins.menu = "Plugins";
    forPlugins.asksUser = true;
    forPlugins.enabled = [](const vela::Document &) { return !vela::CurrentPlugins().waiting.empty(); };
    forPlugins.run = [](vela::Document &, const JsObject &) { return InstallForPlugins(); };
    vela::ActionRegistry::Get().Register(std::move(forPlugins));

    vela::Action reset;
    reset.name = "reset_python_packages";
    reset.title = "Reset Python Packages…";
    reset.description = "Delete the installed Python packages and install again what plugins "
                        "and scripts asked for, e.g. after an install went wrong.";
    reset.menu = "Plugins";
    reset.asksUser = true;
    reset.run = [](vela::Document &, const JsObject &) {
        const std::string base = Base();
        const std::vector<std::string> all = vela::packages::ReadManifest(base).All();
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"Reset Python packages?";
        alert.informativeText = all.empty()
            ? @"Vela deletes the packages it installed. Nothing has asked for packages, so none "
              @"are installed again."
            : [NSString stringWithFormat:@"Vela deletes the packages it installed, then installs "
               @"again what plugins and scripts asked for: %@. Restart Vela afterwards: packages "
               @"already imported stay in use until then.", Join(all)];
        [alert addButtonWithTitle:@"Reset"];
        [alert addButtonWithTitle:@"Cancel"];
        const char *approve = getenv("VELA_PACKAGES_AUTO_APPROVE");
        if (!(approve && *approve == '1') && [alert runModal] != NSAlertFirstButtonReturn) {
            return vela::ActionResult::Error("The user cancelled the reset.");
        }
        for (NSString *folder in @[@"lib", @"bin"]) {
            [NSFileManager.defaultManager removeItemAtPath:[String(base)
                stringByAppendingPathComponent:folder] error:nil];
        }
        vela::Log::Get().Add(vela::LogEntry::Level::Info, "Packages", "Deleted the installed packages");
        if (all.empty()) return vela::ActionResult{};
        NSMutableArray *requirements = [NSMutableArray array];
        for (const std::string &requirement : all) [requirements addObject:String(requirement)];
        NSString *message = nil, *log = nil;
        alreadyConfirmed = true;
        const BOOL ok = VelaInstallPythonPackages(requirements, @"Resetting", &message, &log);
        alreadyConfirmed = false;
        if (!ok) return vela::ActionResult::Error(Text(message));
        return vela::ActionResult{true, JsValue(Text(log ?: @"")), ""};
    };
    vela::ActionRegistry::Get().Register(std::move(reset));

    vela::Action show;
    show.name = "show_python_packages_folder";
    show.title = "Show Python Packages Folder";
    show.description = "Show the folder Vela installs Python packages into, in Finder.";
    show.menu = "Plugins";
    show.run = [](vela::Document &, const JsObject &) {
        NSString *folder = String(Base());
        [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
        [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:folder isDirectory:YES]];
        return vela::ActionResult{true, JsValue(Text(folder)), ""};
    };
    vela::ActionRegistry::Get().Register(std::move(show));
}

void VelaOfferPluginPackages(void) {
    if (vela::CurrentPlugins().waiting.empty()) return;
    const vela::ActionResult result = InstallForPlugins();
    if (!result.ok) vela::Log::Get().Add(vela::LogEntry::Level::Warning, "Packages", result.error);
}
