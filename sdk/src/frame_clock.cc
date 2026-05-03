#include <onyx_anim/sdk/frame_clock.hh>

#include <utility>

namespace onyx_anim {
    frame_clock::frame_clock() noexcept = default;

    void frame_clock::use_audio_clock(time_query q) noexcept {
        if (q) {
            audio_query_ = std::move(q);
            mode_ = mode::audio;
        } else {
            // Detached → fall back to realtime, preserving last known
            // position as the starting offset (so video doesn't visibly
            // jump backwards).
            realtime_pos_ = now();
            audio_query_ = nullptr;
            mode_ = mode::realtime;
        }
    }

    void frame_clock::use_realtime_clock() noexcept {
        // Preserve last known position so a mode switch isn't a seek.
        realtime_pos_ = now();
        audio_query_ = nullptr;
        mode_ = mode::realtime;
    }

    void frame_clock::use_external_clock() noexcept {
        external_pos_ = now();
        audio_query_ = nullptr;
        mode_ = mode::external;
    }

    void frame_clock::tick(std::chrono::microseconds wall_dt) noexcept {
        if (mode_ != mode::realtime) return;
        if (paused_) return;
        if (wall_dt.count() < 0) return;
        realtime_pos_ += wall_dt;
    }

    void frame_clock::set_time(std::chrono::microseconds t) noexcept {
        if (mode_ == mode::realtime) {
            realtime_pos_ = t;
        } else {
            external_pos_ = t;
        }
    }

    void frame_clock::pause() noexcept {
        paused_ = true;
    }

    void frame_clock::resume() noexcept {
        paused_ = false;
    }

    void frame_clock::seek_to(std::chrono::microseconds t) noexcept {
        // In every mode, seek_to forces the next now() to read `t` (until
        // further advancement happens). For audio mode, the engine is
        // additionally expected to seek the audio_stream so the
        // time_query reads the new position; until it does, the audio
        // query may briefly disagree — we cache `t` as a temporary
        // override.
        realtime_pos_ = t;
        external_pos_ = t;
        // Audio mode keeps polling its query but the player will have
        // forced the audio_stream to seek before the next now() call.
    }

    std::chrono::microseconds frame_clock::now() const noexcept {
        switch (mode_) {
            case mode::audio:
                if (audio_query_) return audio_query_();
                return realtime_pos_; // unreachable in normal use
            case mode::realtime:
                return realtime_pos_;
            case mode::external:
                return external_pos_;
        }
        return realtime_pos_;
    }
} // namespace onyx_anim
