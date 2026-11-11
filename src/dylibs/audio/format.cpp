#include "backend.h"
#include <cmath>

namespace mm::audio {
darwin::status parse_format(const darwin::stream_description &f, std::uint32_t maximum_frames, stream_config &out) noexcept {
    constexpr auto flags = darwin::format_float | darwin::format_packed | darwin::format_non_interleaved;
    if (!std::isfinite(f.sample_rate) || f.sample_rate < 8000 || f.sample_rate > 384000 ||
        !f.channels_per_frame || f.channels_per_frame > 32 || !maximum_frames || maximum_frames > 16384 ||
        f.format_id != darwin::linear_pcm || (f.format_flags & ~flags) ||
        (f.format_flags & (darwin::format_float | darwin::format_packed)) != (darwin::format_float | darwin::format_packed) ||
        f.bits_per_channel != 32 || f.frames_per_packet != 1 || f.reserved) return darwin::unsupported_format;
    const auto bytes = 4 * ((f.format_flags & darwin::format_non_interleaved) ? 1 : f.channels_per_frame);
    if (f.bytes_per_frame != bytes || f.bytes_per_packet != bytes) return darwin::unsupported_format;
    out = {f.sample_rate, f.channels_per_frame, maximum_frames,
           (f.format_flags & darwin::format_non_interleaved) ? layout::planar : layout::interleaved};
    return darwin::ok;
}
} // namespace mm::audio
