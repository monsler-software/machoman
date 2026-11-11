#ifndef MACHOMAN_AUDIO_BACKEND_H
#define MACHOMAN_AUDIO_BACKEND_H

#include "darwin_audio_abi.h"
#include <cstdint>
#include <span>

namespace mm::audio {
enum class layout { interleaved, planar };
struct stream_config {
    double sample_rate = 48000;
    std::uint32_t channels = 2, maximum_frames = 512;
    layout buffer_layout = layout::interleaved;
};
enum class cycle_event { normal, underrun, disconnect };
struct cycle {
    std::uint32_t frames;
    std::uint64_t host_time_ns;
    cycle_event event = cycle_event::normal;
};
/* Adapter-owned transport buffers are interleaved float32 for this first
 * increment. Guest layout conversion belongs to the common engine. */
using output_process = darwin::status (*)(void *, const cycle &, std::span<float>) noexcept;
class output_backend {
public:
    virtual ~output_backend() = default;
    virtual darwin::status configure(const stream_config &, output_process, void *) = 0;
    virtual darwin::status start() = 0;
    /* Adapter Stop must quiesce its processing thread before returning.
     * The common engine never waits from inside a render callback. */
    virtual void stop() = 0;
};
darwin::status parse_format(const darwin::stream_description &, std::uint32_t maximum_frames, stream_config &) noexcept;
} // namespace mm::audio
#endif
