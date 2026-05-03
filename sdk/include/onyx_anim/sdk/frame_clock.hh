#pragma once

#include <onyx_anim/sdk/onyx_anim_sdk_export.h>

#include <chrono>
#include <functional>

namespace onyx_anim {
    /**
     * Master clock for video playback. Three sources:
     *
     *   audio   — reads media time from an external function the caller
     *             supplies (typically `io_stream.tell() / bps`). Pause /
     *             seek of the audio stream are reflected automatically;
     *             frame_clock just polls.
     *   realtime — internal monotonic counter advanced by `tick(wall_dt)`
     *             calls. Honours pause / resume.
     *   external — caller drives explicitly via `set_time()`. No internal
     *             advancement. Useful for replay systems or networked
     *             A/V sync where the master clock lives elsewhere.
     *
     * The clock object is single-threaded — it's intended to live inside
     * a player and be ticked from the same thread that calls the
     * decoder. Cross-thread reads are not supported.
     */
    class ONYX_ANIM_SDK_EXPORT frame_clock {
        public:
            enum class mode { audio, realtime, external };

            frame_clock() noexcept;

            // ---- mode selection ----
            //
            // For `audio` mode: pass a function that returns "current media
            // time" in microseconds. Typical usage from inside the player:
            //
            //   clock.use_audio_clock([io = audio_io_stream_,
            //                         bps = bytes_per_second_]
            //                        { return std::chrono::microseconds{
            //                              io->tell() * 1'000'000LL / bps}; });
            //
            // Calling with a nullptr / empty function falls back to
            // realtime mode using the most recently observed time as the
            // starting offset.
            using time_query = std::function<std::chrono::microseconds()>;
            void use_audio_clock(time_query) noexcept;
            void use_realtime_clock() noexcept;
            void use_external_clock() noexcept;

            [[nodiscard]] mode current_mode() const noexcept { return mode_; }

            // ---- realtime advancement ----
            // No-op outside realtime mode. `wall_dt` is the elapsed wall-
            // clock since the last call (positive only — rewinding wall
            // time is undefined).
            void tick(std::chrono::microseconds wall_dt) noexcept;

            // ---- external mode setter ----
            // Sets the current media time directly. In external mode this
            // is the only way to advance; in realtime / audio modes it
            // counts as a seek.
            void set_time(std::chrono::microseconds) noexcept;

            // ---- pause / resume ----
            // Pause freezes the realtime accumulation. In audio mode it
            // is a hint the player can use; the actual audio pause is
            // managed by the engine (which causes the audio time_query
            // to stop advancing naturally).
            void pause() noexcept;
            void resume() noexcept;
            [[nodiscard]] bool is_paused() const noexcept { return paused_; }

            // ---- seek ----
            void seek_to(std::chrono::microseconds t) noexcept;
            void rewind() noexcept { seek_to(std::chrono::microseconds{0}); }

            // ---- read ----
            [[nodiscard]] std::chrono::microseconds now() const noexcept;

        private:
            mode                       mode_     = mode::realtime;
            bool                       paused_   = false;
            std::chrono::microseconds  realtime_pos_{0}; // realtime mode accumulator
            std::chrono::microseconds  external_pos_{0}; // last set_time() in external mode
            time_query                 audio_query_;     // audio mode source
    };
} // namespace onyx_anim
