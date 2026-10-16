// The app's scripting services: the embedded Python and the MCP server on
// its Unix socket, started at launch. The MCP server follows the Settings
// window's switch.
#import <Foundation/Foundation.h>

// Starts the bundle's Python; logs and returns NO if it cannot.
BOOL VelaStartPython(void);

// Starts Python and, when the setting allows it, the MCP server.
void VelaStartScripting(void);

// Whether the MCP server is listening, or why not, for the Settings window.
NSString *VelaMcpStatus(void);
// Posted when VelaMcpStatus changes.
extern NSNotificationName const VelaMcpStatusChanged;
// The Terminal command that adds this app's vela-mcp to Claude Code.
NSString *VelaMcpAddCommand(void);
