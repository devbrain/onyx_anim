#include <onyx_anim/sdk/decoder.hh>

#include <musac/audio_source.hh>

namespace onyx_anim {
    result anim_decoder::open(musac::io_stream* stream) {
        decode_options opts{};
        return open(stream, opts);
    }

    bool anim_decoder::seek_to_frame([[maybe_unused]] unsigned int index) {
        return false;
    }

    bool anim_decoder::seek_to_time([[maybe_unused]] std::chrono::microseconds pts) {
        return false;
    }

    unsigned int anim_decoder::audio_track_count() const noexcept {
        return 0;
    }

    audio_track_info
    anim_decoder::audio_track([[maybe_unused]] unsigned int index) const noexcept {
        return {};
    }

    std::unique_ptr<musac::audio_source>
    anim_decoder::take_audio_track([[maybe_unused]] unsigned int index,
                                   musac::io_stream** io_observer) {
        if (io_observer) *io_observer = nullptr;
        return nullptr;
    }

    std::span<const audio_event> anim_decoder::pending_audio_events() const noexcept {
        return {};
    }
}
