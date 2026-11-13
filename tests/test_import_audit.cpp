#include "machoman/runtime.h"
#include "machoman/venv.h"
#include <cstdio>
#include <cstdlib>

static void check(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "audit: %s\n", message); std::exit(1); }
}
int main(int argc, char **argv) {
    check(argc == 3, "venv and binary arguments");
    mm_venv venv;
    check(mm_venv_open(argv[1], venv) == mm_venv_status::ok, "open venv");
    mm_import_audit report;
    const mm_launch_request request{.venv = &venv, .executable_path = argv[2]};
    check(mm_runtime_audit(request, report) == mm_runtime_status::ok, "complete audit with unresolved imports");
    check(report.guest_images == 1 && report.host_libraries > 0, "dependency closure");
    check(report.missing_dependencies.empty(), "libSystem available");
    bool missing = false, weak = false, host = false;
    for (const auto &entry : report.imports) {
        if (entry.symbol == "_mm_audit_missing") {
            check(entry.resolution == mm_import_resolution::missing && !entry.weak, "required unresolved symbol"); missing = true;
        }
        if (entry.symbol == "_mm_audit_weak") {
            check(entry.resolution == mm_import_resolution::weak_missing && entry.weak, "optional unresolved symbol"); weak = true;
        }
        if (entry.symbol == "_puts") {
            check(entry.resolution == mm_import_resolution::host && !entry.provider.empty(), "host dependency provider"); host = true;
        }
        check(!entry.image.empty() && !entry.dependency.empty(), "symbol provenance");
    }
    check(missing && weak && host, "complete import inventory");
    /* The fixture exits with 98/99 if its initializer or main is executed. */
    std::puts("Import audit resolves dependencies without executing guest code");
}
