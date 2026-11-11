#ifndef MACHOMAN_APPKIT_APP_ICON_H
#define MACHOMAN_APPKIT_APP_ICON_H
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ak {
struct application_icon {
    unsigned width = 0, height = 0;
    std::vector<std::uint32_t> argb; /* premultiplied WL_SHM_FORMAT_ARGB8888 */
};
struct application_identity {
    std::string app_id = "machoman", name = "machoman";
    application_icon icon;
};
application_icon read_application_icon(const std::filesystem::path &path);
bool install_application_icon(const application_identity &identity, const std::filesystem::path &data_home);
const application_identity &main_application_identity();
}
#endif
