// Installing Python packages for plugins and scripts (see core/packages.h):
// the one place installs happen, whoever asks. Vela asks the user first,
// runs the bundled pip in its own process with a progress window (Cancel
// stops it, output goes to the Log), then puts new packages on Python's path
// and loads the plugins that were waiting for them.
#import <Foundation/Foundation.h>

// Installs requirements (PEP 508 strings) together with every requirement
// in the manifest, so the result is consistent. requester says who asks, for
// the confirmation ("The plugin scene_tools"). Returns NO with a message if
// the user declined, a requirement is refused, or pip failed; log gets the
// end of pip's output.
BOOL VelaInstallPythonPackages(NSArray<NSString *> *requirements, NSString *requester,
                               NSString **message, NSString **log);

// install_python_packages, install_plugin_packages, reset_python_packages
// and show_python_packages_folder; call before the menus are built.
void VelaRegisterPackageActions(void);

// At launch: offers to install what waiting plugins need.
void VelaOfferPluginPackages(void);
