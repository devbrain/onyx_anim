#pragma once

#include <onyx_anim/sdk/onyx_anim_sdk_export.h>
#include <onyx_anim/sdk/types.hh>

#include <onyx_image/surface.hpp>
#include <musac/sdk/io_stream.hh>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

// Forward declaration — `take_audio_track` returns a unique_ptr to an
// audio_source; consumers that actually use the returned object include
// <musac/audio_source.hh> themselves.
namespace musac { class audio_source; }

namespace onyx_anim {
    /**
     * Abstract base class for animation decoders.
     *
     * The decoder owns the timeline cursor and is the single source of truth for
     * seek operations. Audio tracks (returned by take_audio_track) are
     * musac::decoder shells that delegate seeking back to this decoder.
     *
     * Threading: open(), info(), decode_frame(), seek_*() are called from the
     * main thread. Audio tracks' do_decode() runs on the musac audio thread.
     * Implementations must serialize shared state with an internal mutex.
     */
    class ONYX_ANIM_SDK_EXPORT anim_decoder {
        public:
            virtual ~anim_decoder() = default;

            // Identification
            [[nodiscard]] virtual std::string_view name() const noexcept = 0;
            [[nodiscard]] virtual std::span <const std::string_view> extensions() const noexcept = 0;

            /**
             * Sniff the stream contents. Implementations must save and restore the
             * stream position so the caller can sniff multiple decoders against the
             * same stream.
             */
            [[nodiscard]] virtual bool sniff(musac::io_stream* stream) const = 0;

            /**
             * Parse headers, build frame index, prepare for decoding.
             * Stream ownership is NOT taken; caller must keep it alive until the
             * decoder is destroyed.
             */
            [[nodiscard]] virtual result open(musac::io_stream* stream,
                                              const decode_options& opts) = 0;

            [[nodiscard]] virtual result open(musac::io_stream* stream);
            [[nodiscard]] virtual const anim_info& info() const noexcept = 0;

            /**
             * Decode the next video frame and write the pixels into `out`. On success
             * returns the metadata of the just-decoded frame.
             *
             * The decoder maintains its own internal surface for delta reconstruction;
             * the caller's surface receives the final, fully-resolved frame.
             */
            [[nodiscard]] virtual frame_result decode_frame(onyx_image::surface& out) = 0;

            [[nodiscard]] virtual bool eof() const noexcept = 0;

            // ------------------------------------------------------------------------
            // Seeking — frame-aligned. Audio resumes at the PTS of the next decoded
            // frame; pre-seek audio in queues is dropped.
            // ------------------------------------------------------------------------

            virtual bool rewind() = 0;
            virtual bool seek_to_frame(unsigned int index);
            virtual bool seek_to_time(std::chrono::microseconds pts);

            // ------------------------------------------------------------------------
            // Audio
            // ------------------------------------------------------------------------

            [[nodiscard]] virtual unsigned int audio_track_count() const noexcept;

            /**
             * Per-track metadata — sample rate, channels, codec name, etc.
             * Cheap to call (does not decode any audio); intended for the
             * "which track should I play?" decision.
             *
             * Default impl returns a zeroed-out struct, so codecs that
             * don't bother to override stay quiet. Returns the same default
             * for out-of-range indices.
             */
            [[nodiscard]] virtual audio_track_info
                audio_track(unsigned int index) const noexcept;

            /**
             * Hand out a ready-to-play `musac::audio_source` for the given
             * audio track. The codec is responsible for pairing its decoder
             * with whatever io_stream feeds the audio bytes — callers just
             * plug the result into `device.create_stream(...)`.
             *
             * Returns nullptr if the track index is invalid or the format has
             * no audio. Each track may only be taken once.
             *
             * If `io_observer` is non-null, the codec writes a non-owning
             * pointer to the audio_source's underlying io_stream into it.
             * The pointer is valid for the lifetime of the returned
             * audio_source — useful for callers (e.g. onyx_anim::player)
             * that want to derive playback time from the io cursor.
             */
            [[nodiscard]] virtual std::unique_ptr<musac::audio_source>
                take_audio_track(unsigned int index,
                                 musac::io_stream** io_observer = nullptr);

            /**
             * Audio events triggered by the most recently decoded frame.
             *
             * Default: empty span (the codec doesn't expose event-driven audio).
             * Codecs that support event-driven sound (e.g. Amiga ANIM+SLA's
             * SCTL chunks) populate this from inside decode_frame() / seek_*().
             *
             * Lifetime: the returned span is only valid until the next call
             * to decode_frame() or seek_*() on this decoder. Each event's
             * `sound_bytes` is shared, so the player may keep playing after
             * the span has been invalidated.
             */
            [[nodiscard]] virtual std::span<const audio_event>
                pending_audio_events() const noexcept;
    };
} // namespace onyx_anim
