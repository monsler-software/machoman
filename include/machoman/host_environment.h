#ifndef MACHOMAN_HOST_ENVIRONMENT_H
#define MACHOMAN_HOST_ENVIRONMENT_H
#include "machoman/runtime.h"
#include <cstdlib>
#include <cstring>

/* Guest-facing framework/libSystem APIs use these helpers. Host display,
 * portal, tracing and other backend configuration still use host getenv. */
inline char **mm_host_environment(const mm_host_api *api) {
    return api && api->version >= 6 && api->environment ? *api->environment() : api ? api->envp : nullptr;
}
inline char *mm_host_getenv(const mm_host_api *api, const char *name) {
    if (api && api->version >= 6 && api->environment_get) return api->environment_get(name);
    return std::getenv(name);
}
inline const char *mm_host_venv(const mm_host_api *api) {
    return api && api->version >= 6 ? api->venv_root : std::getenv("MACHOMAN_VENV");
}
inline const char *mm_host_launcher(const mm_host_api *api) {
    return api && api->version >= 6 ? api->launcher_path : std::getenv("MACHOMAN_EXE");
}
/* Re-enter the runtime without leaking launch transport into guest envp.
 * Keep the original argv[0], including "sh" and login-shell names. */
inline std::vector<std::string> mm_guest_launch_arguments(const mm_host_api *api,
    const std::string &program, char *const *argv, std::uint32_t mask) {
    const char *launcher=mm_host_launcher(api), *venv=mm_host_venv(api);
    if (!launcher || !venv || !argv || !argv[0]) return {};
    std::vector<std::string> args={launcher, "run", "--exact-env", "--argv0", argv[0],
        "--signal-mask", std::to_string(mask), venv, program};
    for (auto p=argv+1; *p; ++p) args.emplace_back(*p);
    return args;
}
#endif
