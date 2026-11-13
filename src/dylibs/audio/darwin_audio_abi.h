#ifndef MACHOMAN_DARWIN_AUDIO_ABI_H
#define MACHOMAN_DARWIN_AUDIO_ABI_H

#include <cstddef>
#include <cstdint>

namespace mm::audio::darwin {
using status = std::int32_t;
constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return (std::uint32_t(a) << 24) | (std::uint32_t(b) << 16) | (std::uint32_t(c) << 8) | std::uint32_t(d);
}
constexpr std::uint32_t tag(const char (&s)[5]) { return fourcc(s[0], s[1], s[2], s[3]); }
inline constexpr status ok = 0, bad_size = fourcc('!', 's', 'i', 'z'),
    illegal_operation = fourcc('n', 'o', 'p', 'e'), unsupported_format = fourcc('!', 'd', 'a', 't'),
    bad_object = fourcc('!', 'o', 'b', 'j'), bad_device = fourcc('!', 'd', 'e', 'v'),
    unknown_property = fourcc('w', 'h', 'o', '?'), bad_parameter = -50;
inline constexpr std::uint32_t linear_pcm = fourcc('l', 'p', 'c', 'm');
inline constexpr std::uint32_t format_float = 1, format_big_endian = 2, format_signed_integer = 4,
    format_packed = 8, format_aligned_high = 16, format_non_interleaved = 32;
inline constexpr std::uint32_t sample_time_valid = 1, host_time_valid = 2, rate_scalar_valid = 4;

struct property_address { std::uint32_t selector, scope, element; };
struct stream_description {
    double sample_rate;
    std::uint32_t format_id, format_flags, bytes_per_packet, frames_per_packet,
        bytes_per_frame, channels_per_frame, bits_per_channel, reserved;
};
struct buffer { std::uint32_t channels, byte_size; void *data; };
struct buffer_list { std::uint32_t count; buffer buffers[1]; };
struct smpte_time {
    std::int16_t subframes, subframe_divisor;
    std::uint32_t counter, type, flags;
    std::int16_t hours, minutes, seconds, frames;
};
struct timestamp {
    double sample_time;
    std::uint64_t host_time;
    double rate_scalar;
    std::uint64_t word_clock_time;
    smpte_time smpte;
    std::uint32_t flags, reserved;
};
struct value_range { double minimum, maximum; };
struct ranged_description { stream_description format; value_range sample_rate_range; };
struct component_description { std::uint32_t type, subtype, manufacturer, flags, flags_mask; };
struct render_callback { std::uint64_t function, refcon; };

static_assert(sizeof(property_address) == 12 && alignof(property_address) == 4);
static_assert(sizeof(stream_description) == 40 && offsetof(stream_description, reserved) == 36);
static_assert(sizeof(buffer) == 16 && offsetof(buffer, data) == 8);
static_assert(sizeof(buffer_list) == 24 && offsetof(buffer_list, buffers) == 8);
static_assert(sizeof(smpte_time) == 24);
static_assert(sizeof(timestamp) == 64 && offsetof(timestamp, smpte) == 32 && offsetof(timestamp, flags) == 56);
static_assert(sizeof(value_range) == 16 && sizeof(component_description) == 20 && sizeof(render_callback) == 16);
} // namespace mm::audio::darwin
#endif
