// The app's side of plugins (see core/plugins.h): the Plugins menu's Show
// Plugins Folder, loading plugins once Vela has launched, and their menus.
#import <Foundation/Foundation.h>

// Registers show_plugins_folder; call before the menus are built.
void VelaRegisterPluginAppActions(void);
// Loads plugins (if there are any) soon after launch, once Python runs, and
// keeps their menus up to date.
void VelaStartPlugins(void);
