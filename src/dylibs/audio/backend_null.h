#ifndef MACHOMAN_AUDIO_BACKEND_NULL_H
#define MACHOMAN_AUDIO_BACKEND_NULL_H
#include "backend.h"
#include <thread>

namespace mm::audio {
/* Explicit offline driver. It never becomes a default physical device.
 * Configure, Start, Pump and Stop are serialized on its owning thread. */
class null_backend final : public output_backend {
public:
    darwin::status configure(const stream_config &, output_process, void *) override;
    darwin::status start() override;
    void stop() override;
    darwin::status pump(const cycle &, std::span<float>) noexcept;
private:
    std::thread::id owner_{};
    stream_config config_;
    output_process process_ = nullptr;
    void *refcon_ = nullptr;
    bool running_ = false, processing_ = false;
};
} // namespace mm::audio
#endif
