#ifndef MACHOMAN_AUDIO_BACKEND_PIPEWIRE_H
#define MACHOMAN_AUDIO_BACKEND_PIPEWIRE_H

#include "backend.h"
#include <memory>
#include <string>
#include <vector>

namespace mm::audio {
struct output_device {
    std::uint32_t id = 0, node_id = 0, channels = 2;
    std::string serial, name, description;
    int priority = 0;
};
struct device_snapshot {
    std::vector<output_device> outputs;
    std::uint32_t default_output = 0;
};

/* Only endpoints discovered from the running server are returned. IDs are
 * process-local and never reused after removal. A missing server is empty. */
device_snapshot pipewire_devices();

/* An empty target follows the session manager's default output. An explicit
 * target is its object serial (not a recyclable PipeWire node ID). */
std::unique_ptr<output_backend> make_pipewire_output(std::string target, worker_hooks = {}, std::string application = "machoman");
} // namespace mm::audio
#endif
