#ifndef MACHOMAN_AUDIO_OUTPUT_SESSION_H
#define MACHOMAN_AUDIO_OUTPUT_SESSION_H

#include "backend_pipewire.h"
#include "engine.h"
#include <atomic>
#include <memory>
#include <mutex>

struct mm_host_api;
namespace mm::audio {
enum class callback_kind { io_proc, audio_unit };

/* The facade serializes control calls and keeps this object alive until Stop
 * has joined the worker. A callback may Stop itself, but may not dispose it. */
class output_session {
public:
    output_session() = default;
    ~output_session();
    darwin::status initialize(const stream_config &, darwin::render_callback, callback_kind,
                              std::uint32_t device, std::string target, std::atomic<unsigned> *active,
                              std::uint64_t clock_origin = 0);
    darwin::status start();
    void stop();
    void set_active_counter(std::atomic<unsigned> *);
    bool running() const noexcept { return engine_.state() == stream_state::running; }
    bool in_callback() const noexcept { return current_ == this; }
    static bool is_current(const output_session *session) noexcept { return session && current_ == session; }
    static bool on_audio_thread() noexcept { return current_ != nullptr; }
    darwin::status last_error() const noexcept { return error_.load(std::memory_order_acquire); }
private:
    static darwin::status prepare(void *);
    static void finish(void *);
    static darwin::status process(void *, const cycle &, std::span<float>) noexcept;
    static darwin::status render(void *, const darwin::timestamp &, std::uint32_t, darwin::buffer_list *) noexcept;
    static thread_local output_session *current_;
    output_engine engine_;
    std::unique_ptr<output_backend> backend_;
    stream_config config_;
    darwin::render_callback callback_{};
    callback_kind kind_ = callback_kind::io_proc;
    std::uint32_t device_ = 0;
    std::uint64_t clock_origin_ = 0;
    const mm_host_api *api_ = nullptr;
    void *prepared_ = nullptr;
    std::vector<float> fixed_output_;
    std::uint32_t pending_frames_ = 0, pending_offset_ = 0;
    std::mutex activity_mutex_;
    bool counted_ = false;
    std::atomic<unsigned> *active_ = nullptr;
    std::atomic<darwin::status> error_{0};
};
} // namespace mm::audio
#endif
