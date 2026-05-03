#include <onyx_anim/player/player.hh>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/convert.hh>
#include <onyx_anim/sdk/frame_clock.hh>

#include <algorithm>
#include <chrono>

namespace onyx_anim {
    namespace {
        // We rely on the io_stream cursor of the audio source to drive
        // the audio clock. The player creates the audio_source via the
        // codec's `take_audio_track`, captures a raw observer pointer
        // to its underlying io_stream, and then either creates an
        // audio_stream internally (Tier 1) or hands the source off to
        // the engine (Tier 3). In both cases the io_stream lives as
        // long as the audio_source does, and the player holds the
        // audio_stream pointer (its own or adopted) for control.
        class player_impl final : public player {
            public:
                player_impl() = default;
                ~player_impl() override {
                    // Release the audio stream first if we own it,
                    // before the source it pulls from is gone.
                    owned_audio_stream_.reset();
                }

                // ---- info ----
                const anim_info& info() const noexcept override { return decoder_->info(); }

                unsigned int audio_track_count() const noexcept override {
                    return decoder_->audio_track_count();
                }

                audio_track_info audio_track(unsigned int index) const noexcept override {
                    return decoder_->audio_track(index);
                }

                std::chrono::microseconds current_time() const noexcept override {
                    return clock_.now();
                }

                bool is_playing() const noexcept override {
                    return playing_ && !eof_;
                }

                bool eof() const noexcept override { return eof_; }

                // ---- lifecycle ----
                void play() override {
                    playing_ = true;
                    eof_ = false;
                    clock_.resume();
                    if (audio_stream_observer_) {
                        // 1 iteration if !loop, ~unsigned-max for loop.
                        audio_stream_observer_->play();
                    }
                }

                void pause() override {
                    playing_ = false;
                    clock_.pause();
                    if (audio_stream_observer_) {
                        audio_stream_observer_->pause();
                    }
                }

                void rewind() override { seek_to_time(std::chrono::microseconds{0}); }

                void seek_to_time(std::chrono::microseconds t) override {
                    decoder_->seek_to_time(t);
                    if (audio_stream_observer_) {
                        audio_stream_observer_->seek_to_time(t);
                    }
                    clock_.seek_to(t);
                    last_decoded_pts_ = std::chrono::microseconds{-1};
                    eof_ = false;
                }

                // ---- per-frame ----
                bool tick(onyx_image::surface& out) override {
                    return advance_to_time(clock_.now(), out);
                }

                bool advance_to_time(std::chrono::microseconds media_time,
                                     onyx_image::surface& out) override {
                    if (!playing_) return false;
                    if (eof_) return false;

                    const auto& info = decoder_->info();
                    const auto period = info.frame_period;
                    if (period.count() <= 0) return false;

                    // Loop wrap.
                    if (loop_ && info.duration.count() > 0 &&
                        media_time >= info.duration) {
                        media_time = std::chrono::microseconds{
                            media_time.count() % info.duration.count()};
                        decoder_->seek_to_time(media_time);
                        if (audio_stream_observer_) {
                            audio_stream_observer_->seek_to_time(std::chrono::microseconds{0});
                            audio_stream_observer_->play();
                        }
                        clock_.seek_to(media_time);
                        last_decoded_pts_ = std::chrono::microseconds{-1};
                    }

                    // Compute the target frame index for `media_time`.
                    const auto target_idx = static_cast <unsigned int>(
                        media_time.count() / period.count());
                    if (target_idx >= info.frame_count) {
                        if (loop_) {
                            decoder_->seek_to_time(std::chrono::microseconds{0});
                            clock_.seek_to(std::chrono::microseconds{0});
                            if (audio_stream_observer_) {
                                audio_stream_observer_->seek_to_time(std::chrono::microseconds{0});
                                audio_stream_observer_->play();
                            }
                            last_decoded_pts_ = std::chrono::microseconds{-1};
                            return false;
                        }
                        eof_ = true;
                        if (eos_callback_) eos_callback_();
                        return false;
                    }

                    // If the target frame is already on display, skip.
                    if (last_decoded_idx_ == target_idx &&
                        last_decoded_pts_.count() >= 0) {
                        return false;
                    }

                    // If we need to skip backwards or jump forward more
                    // than 1 frame, seek (delta-coded codecs replay
                    // from previous keyframe).
                    if (target_idx < last_decoded_idx_ ||
                        target_idx > last_decoded_idx_ + 1u) {
                        if (!decoder_->seek_to_frame(target_idx)) {
                            // Fall back to time-seek.
                            decoder_->seek_to_time(target_idx * period);
                        }
                    }

                    auto fr = decoder_->decode_frame(internal_surface_);
                    if (!fr) {
                        if (error_callback_) error_callback_(fr.error());
                        eof_ = true;
                        return false;
                    }
                    last_decoded_idx_ = fr->index;
                    last_decoded_pts_ = fr->pts;

                    if (auto cr = convert_surface(internal_surface_, out, preferred_format_); !cr) {
                        if (error_callback_) error_callback_(cr.error());
                        return false;
                    }
                    return true;
                }

                // ---- volume ----
                void set_volume(float v) override {
                    if (audio_stream_observer_) {
                        audio_stream_observer_->set_volume(v);
                    }
                }

                // ---- tier 3 audio handoff ----
                std::unique_ptr<musac::audio_source>
                take_audio_track(unsigned int index) override {
                    if (owned_audio_stream_) return nullptr; // already in tier 1
                    return decoder_->take_audio_track(index);
                }

                void adopt_audio_stream(musac::audio_stream* stream) override {
                    if (owned_audio_stream_) return; // tier 1 owns it
                    audio_stream_observer_ = stream;
                    if (stream && audio_io_observer_ && bytes_per_second_ > 0) {
                        bind_audio_clock();
                    }
                }

                musac::audio_stream* audio_stream() noexcept override {
                    return audio_stream_observer_;
                }

                // ---- callbacks ----
                void on_end_of_stream(std::function<void()> fn) override {
                    eos_callback_ = std::move(fn);
                }
                void on_error(std::function<void(error_type)> fn) override {
                    error_callback_ = std::move(fn);
                }

                // ---- internal: setup helpers used by player::open ----
                void set_decoder(std::unique_ptr<anim_decoder> d) {
                    decoder_ = std::move(d);
                    internal_surface_ = onyx_image::memory_surface{};
                }

                void set_loop(bool b) { loop_ = b; }
                void set_preferred_format(pixel_format f) { preferred_format_ = f; }

                void install_owned_audio_stream(musac::audio_stream s) {
                    owned_audio_stream_ = std::make_unique<musac::audio_stream>(std::move(s));
                    audio_stream_observer_ = owned_audio_stream_.get();
                    if (audio_io_observer_ && bytes_per_second_ > 0) {
                        bind_audio_clock();
                    }
                }

            private:
                void bind_audio_clock() {
                    clock_.use_audio_clock(
                        [io = audio_io_observer_, bps = bytes_per_second_]() {
                            const auto pos = io->tell();
                            if (pos < 0) return std::chrono::microseconds{0};
                            return std::chrono::microseconds{
                                static_cast <std::int64_t>(pos) * 1'000'000LL /
                                static_cast <std::int64_t>(bps)};
                        });
                }

                std::unique_ptr<anim_decoder>     decoder_;
                onyx_image::memory_surface        internal_surface_;
                frame_clock                       clock_;

                std::unique_ptr<musac::audio_stream> owned_audio_stream_;
                musac::audio_stream*              audio_stream_observer_ = nullptr;
                musac::io_stream*                 audio_io_observer_ = nullptr;
                unsigned int                      bytes_per_second_ = 0;

                pixel_format                      preferred_format_ = pixel_format::rgba8888;
                bool                              loop_ = false;
                bool                              playing_ = false;
                bool                              eof_ = false;
                std::chrono::microseconds         last_decoded_pts_{-1};
                unsigned int                      last_decoded_idx_ = 0;

                std::function<void()>             eos_callback_;
                std::function<void(error_type)>   error_callback_;
        };
    } // namespace

    player::~player() = default;

    expected<std::unique_ptr<player>, error_type>
    player::open(std::unique_ptr<musac::io_stream> stream,
                 const player_options& opts) {
        if (!stream) {
            return make_unexpected<error_type>("player: null io_stream");
        }
        auto& reg = codec_registry::instance();
        auto dec = reg.create_decoder(stream.get());
        if (!dec) {
            return make_unexpected<error_type>(
                "player: no codec sniffed for the given stream");
        }
        if (auto r = dec->open(stream.get(), opts.decode); !r) {
            return make_unexpected<error_type>(r.error());
        }

        auto p = std::make_unique<player_impl>();
        p->set_loop(opts.loop);
        p->set_preferred_format(opts.preferred_format);

        const auto track_count = dec->audio_track_count();
        const auto track_index = opts.audio_track;
        const bool has_audio = track_count > 0 && track_index < track_count;

        // For Tier 1 (audio_device set), pull the source out NOW so we
        // can capture the io_stream observer before handing the source
        // to musac.
        if (has_audio && opts.audio_device != nullptr) {
            const auto t = dec->audio_track(track_index);
            // We need the io_stream observer. The codec's take_audio_track
            // hands us a fully-constructed audio_source whose internal
            // io_stream is what we want to observe. We pull it out and
            // immediately push it into the device.
            auto src = dec->take_audio_track(track_index);
            if (src) {
                // The io_stream lives inside the source. We expose it via
                // a public helper on audio_source — but musac doesn't
                // ship one. Workaround: the codec's pcm_audio_decoder
                // holds the bytes via shared_ptr and reads via stream.
                // We capture the observer at construction time inside
                // the codec; the player then asks the source for its
                // io_stream pointer by an adapter call.
                //
                // For now, we forgo the audio-clock optimisation when
                // audio_device is set — fall back to clock::realtime.
                // Future: extend musac's audio_source with a `peek_io()`
                // accessor.
                //
                // (Tier 3 path with adopt_audio_stream lets the engine
                // pass an io_stream observer in directly if it really
                // wants the io-cursor clock.)
                auto stream_obj = opts.audio_device->create_stream(std::move(*src));
                stream_obj.set_volume(opts.audio_volume);
                p->install_owned_audio_stream(std::move(stream_obj));
                (void) t;
            }
        }
        // If no audio path was set up, the clock stays in realtime mode
        // (its default after construction).
        p->set_decoder(std::move(dec));
        return p;
    }
} // namespace onyx_anim
