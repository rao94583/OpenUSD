#import "scripting.h"
#import "document.h"

#import <AppKit/AppKit.h>

#include "app/vela/core/app_settings.h"
#include "app/vela/core/mcp_socket.h"
#include "app/vela/core/python.h"
#include "app/vela/core/session.h"

#include <memory>

NSNotificationName const VelaMcpStatusChanged = @"VelaMcpStatusChanged";

namespace {

std::unique_ptr<vela::McpSocketServer> &Server() {
    static std::unique_ptr<vela::McpSocketServer> server;
    return server;
}

NSString *status = @"The MCP server is off.";

void SetStatus(NSString *text) {
    status = text;
    [NSNotificationCenter.defaultCenter postNotificationName:VelaMcpStatusChanged object:nil];
}

// Runs MCP messages on the main thread, where documents and AppKit live,
// in whatever mode the run loop is in (menus and sheets included).
void RunOnMain(const std::function<void()> &function) {
    if (NSThread.isMainThread) {
        function();
        return;
    }
    dispatch_sync(dispatch_get_main_queue(), ^{ function(); });
}

void UpdateMcpServer() {
    auto &server = Server();
    const bool wanted = vela::AppSettingsStore::Get().GetSettings().mcpServer;
    if (!wanted) {
        if (server) {
            server.reset();
            SetStatus(@"The MCP server is off.");
        }
        return;
    }
    if (server && server->IsListening()) return;
    server = std::make_unique<vela::McpSocketServer>(vela::DefaultMcpSocketPath(), RunOnMain);
    std::string error;
    if (server->Start(&error)) {
        SetStatus([NSString stringWithFormat:@"Listening at %@.", @(server->GetPath().c_str())]);
    } else {
        server.reset();
        NSLog(@"MCP server: %s", error.c_str());
        SetStatus([NSString stringWithFormat:@"Not listening: %@", @(error.c_str())]);
    }
}

// Opens a file as File → Open does, for vela.open() and open_document.
int OpenForScript(const std::string &path, std::string *error) {
    NSURL *url = [NSURL fileURLWithPath:@(path.c_str())];
    NSDocumentController *controller = NSDocumentController.sharedDocumentController;
    NSDocument *document = [controller documentForURL:url];
    if (!document) {
        NSError *failure = nil;
        NSString *type = [controller typeForContentsOfURL:url error:&failure];
        if (type) document = [controller makeDocumentWithContentsOfURL:url ofType:type error:&failure];
        if (!document) {
            *error = failure ? failure.localizedDescription.UTF8String : "Could not open " + path;
            if (NSString *reason = failure.localizedFailureReason) {
                *error += std::string(": ") + reason.UTF8String;
            }
            return 0;
        }
        [controller addDocument:document];
        [document makeWindowControllers];
    }
    [document showWindows];
    if (![document isKindOfClass:[VelaDocument class]]) return 0;
    return ((VelaDocument *)document).sessionId;
}

} // namespace

BOOL VelaStartPython(void) {
    NSString *runtime = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"Runtime"];
    std::string error;
    if (!vela::InitializePython([runtime stringByAppendingPathComponent:@"python"].UTF8String,
                                {[runtime stringByAppendingPathComponent:@"usd/lib/python"].UTF8String},
                                &error)) {
        NSLog(@"Python: %s", error.c_str());
        return NO;
    }
    return YES;
}

void VelaStartScripting(void) {
    VelaStartPython();
    vela::Session::Get().open = OpenForScript;
    UpdateMcpServer();
    vela::AppSettingsStore::Get().AddObserver([] { UpdateMcpServer(); });
    // Leave no socket behind.
    [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationWillTerminateNotification
        object:nil queue:nil usingBlock:^(NSNotification *) { Server().reset(); }];
}

NSString *VelaMcpStatus(void) { return status; }

NSString *VelaMcpAddCommand(void) {
    NSString *helper = [NSBundle.mainBundle.executablePath.stringByDeletingLastPathComponent
                        stringByAppendingPathComponent:@"vela-mcp"];
    return [NSString stringWithFormat:@"claude mcp add vela -- '%@'", helper];
}
