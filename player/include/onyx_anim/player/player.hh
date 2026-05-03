#pragma once

#include <onyx_anim/player/onyx_anim_player_export.h>
#include <onyx_anim/sdk/decoder.hh>
#include <onyx_anim/sdk/types.hh>

#include <onyx_image/surface.hpp>

#include <musac/audio_device.hh>
#include <musac/audio_source.hh>
#include <musac/sdk/io_stream.hh>
#include <musac/stream.hh>

#include <chrono>
#include <functional>
#include <memory>
#include <span>

namespace onyx_anim {
    /**
     * Construction-time configuration for `player`.
     *
     * The single most important field is `audio_device`. If set, the
     * player creates and owns an internal `musac::audio_stream` for the
     * selected audio track and drives all of play / pause / seek /
     * volume through it (Tier 1 — drop-in).
     *
     * If `audio_device` is null, the player keeps the audio source
     * unused (engine handles audio out-of-band) — call `take_audio_track`
     * to get the source and `adopt_audio_stream` to register the engine-
     * owned stream back with the player so the audio clock and lifecycle
     * coordination still work (Tier 3 — full control).
     */
    struct player_options {
        bool         loop = false;
        pixel_format preferred_format = pixel_format::rgba8888;

        /// If non-null, the player creates an internal audio_stream from
        /// this device for the selected audio track.
        musac::audio_device* audio_device = nullptr;

        /// Which audio track to play (most files have only track 0).
        unsigned int audio_track = 0;

        /// Initial volume for the internally-created audio_stream.
        /// Ignored when `audio_device` is null.
        float        audio_volume = 1.0f;

        /// When true (and `audio_device` is set), the player automatically
        /// spawns one-shot streams for each `audio_event` reported by the
        /// codec (e.g. ANIM+SLA SCTL triggers) on every tick that decodes
        /// a new frame. Engines that want manual control set this false
        /// and call `pending_audio_events()` themselves.
        bool         auto_fire_audio_events = true;

        /// Cap on concurrent event-driven streams. Streams that have
        /// finished playing are pruned each tick; new triggers past the
        /// cap are dropped. Ignored unless `auto_fire_audio_events`.
        unsigned int max_concurrent_event_streams = 16;

        /// Forwarded to anim_decoder::open. Engines wanting non-default
        /// max dimensions / preferred codec format can tweak it.
        decode_options decode = {};
    };

    /**
     * Engine-facing video player. Wraps an `anim_decoder`, an optional
     * `musac::audio_stream`, and a `frame_clock` to expose a one-call-
     * per-game-frame API:
     *
     *     auto p = player::open(std::move(stream), { .audio_device = &dev });
     *     p->play();
     *     while (running) {
     *         if (p->tick(my_surface)) upload(my_surface);
     *     }
     *
     * The player holds an internal `memory_surface` that the codec
     * decodes into, then converts to the engine's preferred pixel
     * format (`player_options::preferred_format`) on each tick that
     * produces a new frame. Engines provide their own `onyx_image::surface`
     * subclass; the player never allocates inside the engine's surface.
     *
     * Threading: single-threaded. All public methods must be called from
     * the same thread. The internal audio_stream is fed by musac's audio
     * thread, but the player only observes its io_stream cursor — no
     * shared mutable state on the hot path.
     */
    class ONYX_ANIM_PLAYER_EXPORT player {
        public:
            virtual ~player();

            static expected<std::unique_ptr<player>, error_type> open(
                std::unique_ptr<musac::io_stream> stream,
                const player_options& opts = {});

            // ---- info ----
            [[nodiscard]] virtual const anim_info& info() const noexcept = 0;
            [[nodiscard]] virtual unsigned int audio_track_count() const noexcept = 0;
            [[nodiscard]] virtual audio_track_info audio_track(unsigned int index) const noexcept = 0;
            [[nodiscard]] virtual std::chrono::microseconds current_time() const noexcept = 0;
            [[nodiscard]] virtual bool is_playing() const noexcept = 0;
            [[nodiscard]] virtual bool eof() const noexcept = 0;

            // ---- lifecycle ----
            virtual void play() = 0;
            virtual void pause() = 0;
            virtual void rewind() = 0;
            virtual void seek_to_time(std::chrono::microseconds) = 0;

            // ---- per-frame ----
            // Returns true when a new frame was rendered into `out`,
            // false when the surface was not touched (paused, eof,
            // not-enough-time-elapsed). Errors flow via on_error
            // callback.
            [[nodiscard]] virtual bool tick(onyx_image::surface& out) = 0;
            [[nodiscard]] virtual bool advance_to_time(
                std::chrono::microseconds media_time,
                onyx_image::surface& out) = 0;

            // ---- audio (Tier 1 — when audio_device was set) ----
            virtual void set_volume(float) = 0;

            // ---- audio (Tier 3 — engine wants full control) ----
            // Take the audio source out of the player. The player will
            // then track the resulting audio_stream via adopt_audio_stream
            // for clock/lifecycle. Returns nullptr if audio_device was
            // already set in options.
            //
            // If `io_observer` is non-null, the player writes a non-owning
            // pointer to the audio_source's underlying io_stream into it.
            // The pointer is valid for the lifetime of the returned
            // audio_source — engines can use it to drive an audio clock
            // by reading the byte cursor.
            [[nodiscard]] virtual std::unique_ptr<musac::audio_source>
                take_audio_track(unsigned int index = 0,
                                 musac::io_stream** io_observer = nullptr) = 0;
            virtual void adopt_audio_stream(musac::audio_stream*) = 0;

            /// Direct access to the currently-bound audio_stream, if any
            /// (whether internally created in Tier 1 or engine-adopted in
            /// Tier 3). Useful for engines wanting to add fades / route
            /// effects beyond what the player exposes directly.
            [[nodiscard]] virtual musac::audio_stream* audio_stream() noexcept = 0;

            /// Per-frame event-driven audio triggers (e.g. Amiga ANIM+SLA
            /// SCTL chunks). Engines hand each event's `sound_bytes` to
            /// `musac::io_from_memory` + `audio_source` to spawn a one-
            /// shot stream. The returned span is invalidated by the next
            /// `tick` / `advance_to_time` / `seek_to_time`.
            [[nodiscard]] virtual std::span<const audio_event>
                pending_audio_events() const noexcept = 0;

            // ---- callbacks ----
            virtual void on_end_of_stream(std::function<void()>) = 0;
            virtual void on_error(std::function<void(error_type)>) = 0;
    };
} // namespace onyx_anim
