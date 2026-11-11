#ifndef MACHOMAN_AUDIO_ENGINE_H
#define MACHOMAN_AUDIO_ENGINE_H
#include "backend.h"
#include <atomic>
#include <vector>

namespace mm::audio {
enum class stream_state { created, configured, initialized, running, stopped, error, disposed };
using render_function = darwin::status (*)(void *, const darwin::timestamp &, std::uint32_t, darwin::buffer_list *) noexcept;

/* Control operations except Stop are serialized and require quiescence.
 * One adapter processing thread owns each stream. No buffer allocation,
 * mutex, logging or reference-count destruction occurs in process(). */
class output_engine {
public:
    darwin::status configure(const stream_config &, render_function, void *);
    darwin::status initialize();
    darwin::status start() noexcept;
    void stop() noexcept;
    darwin::status dispose();
    darwin::status process(const cycle &, std::span<float>) noexcept;
    static darwin::status process_adapter(void *self, const cycle &c, std::span<float> out) noexcept {
        return static_cast<output_engine *>(self)->process(c, out);
    }
    stream_state state() const noexcept { return state_.load(std::memory_order_acquire); }
    bool quiescent() const noexcept { return !processing_.load(std::memory_order_acquire); }
    std::uint64_t frame_position() const noexcept { return frame_position_; }
    std::uint64_t underruns() const noexcept { return underruns_; }
    std::uint64_t callback_failures() const noexcept { return callback_failures_; }
private:
    std::atomic<stream_state> state_{stream_state::created};
    std::atomic<bool> processing_{false};
    stream_config config_;
    render_function render_ = nullptr;
    void *refcon_ = nullptr;
    std::vector<float> samples_;
    std::vector<std::uint64_t> list_storage_;
    std::uint64_t frame_position_ = 0, underruns_ = 0, callback_failures_ = 0;
};
} // namespace mm::audio
#endif
