#ifndef MACHOMAN_COREAUDIO_HAL_H
#define MACHOMAN_COREAUDIO_HAL_H

#include "backend_pipewire.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace mm::audio {
struct hal_device {
    output_device info;
    std::mutex mutex;
    double sample_rate = 48000;
    std::uint32_t buffer_frames = 512;
    std::uint64_t clock_origin = 0;
    std::atomic<unsigned> active{0};
};
std::shared_ptr<hal_device> find_hal_device(std::uint32_t);
std::uint32_t default_hal_output();
darwin::stream_description hal_format(const hal_device &);
void notify_audio_property(std::uint32_t object, darwin::property_address);
void ensure_audio_device_monitor();
void refresh_audio_units(const device_snapshot &);
void quiesce_audio_hal();
void quiesce_audio_units();
void quiesce_audio_listeners();
} // namespace mm::audio
#endif
