#ifndef MACHOMAN_INSTALL_H
#define MACHOMAN_INSTALL_H
#include "machoman/venv.h"
#include <vector>

struct mm_installed_app {
    std::filesystem::path bundle;
    std::string executable;
};
struct mm_install_result {
    std::vector<mm_installed_app> apps;
    std::string error;
};
struct mm_install_options {
    /* Optional explicit tool. Otherwise use MACHOMAN_7ZIP, PATH, then
     * tools/ beside the CLI executable. */
    std::filesystem::path seven_zip, executable_directory;
};
/* Install outermost .app bundles into an existing venv. Reject conflicts
 * and publish complete bundles/launchers after metadata and ARM64 checks. */
bool mm_install_dmg(const mm_venv &, const std::filesystem::path &dmg,
                    mm_install_result &, const mm_install_options & = {});
#endif
