#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <anim/decoders.hh>
#include <anim/header.hh>

#include <iff/chunk_iterator.hh>
#include <iff/fourcc.hh>
#include <iff/handler_registry.hh>
#include <iff/parse_options.hh>
#include <iff/parser.hh>

#include <musac/audio_source.hh>
#include <musac/sdk/decoder.hh>

#include "amiga_anim.hh"

namespace onyx_anim {
    namespace {
        constexpr std::array<std::string_view, 1> kAnimExtensions = {".anim"};

        using iff::operator""_4cc;
        constexpr auto kFormCC = "FORM"_4cc;
        constexpr auto kAnimCC = "ANIM"_4cc;
        constexpr auto kIlbmCC = "ILBM"_4cc;
        constexpr auto kBmhdCC = "BMHD"_4cc;
        constexpr auto kCmapCC = "CMAP"_4cc;
        constexpr auto kCamgCC = "CAMG"_4cc;
        constexpr auto kBodyCC = "BODY"_4cc;
        constexpr auto kAnhdCC = "ANHD"_4cc;
        constexpr auto kDltaCC = "DLTA"_4cc;
        constexpr auto kSxhdCC = "SXHD"_4cc;
        constexpr auto kSbdyCC = "SBDY"_4cc;
        constexpr auto kSctlCC = "SCTL"_4cc;
        constexpr auto k8svxCC = "8SVX"_4cc;

        // Per-frame raw chunk data, captured during the IFF walk in open().
        struct frame_record {
            std::optional<anim::bmhd>      bmhd;
            std::optional<anim::anhd>      anhd;
            std::optional<std::uint32_t>   camg;
            std::optional<std::vector<std::uint8_t>> cmap_rgb;  // already BMHD-RGB
            std::vector<std::uint8_t>      body;   // raw ByteRun1 stream (keyframe)
            std::vector<std::uint8_t>      dlta;   // raw delta stream
            std::vector<std::uint8_t>      sbdy;   // raw audio bytes for this frame
            std::vector<audio_event>       audio_events;  // ANIM+SLA SCTL triggers
        };

        // ANIM+SLA "Sound ConTroL" — 16 bytes, big-endian. Mirrors the layout
        // documented in iffanimplay's iffanim_audio.hpp.
        struct sctl {
            std::uint8_t  command;
            std::uint8_t  volume;     // 0..64
            std::uint16_t sound;      // 1-indexed slot in the 8SVX bank
            std::uint16_t repeats;    // 0 = loop forever
            std::uint16_t channel;    // bitmask
            std::uint16_t frequency;  // 0 = use VHDR rate
            std::uint16_t flags;
            // 4 trailing pad bytes ignored
        };

        [[nodiscard]] sctl parse_sctl(std::span<const std::uint8_t> data) {
            sctl s{};
            if (data.size() < 12u) return s;
            anim::byte_reader br{data};
            br >> s.command >> s.volume
               >> s.sound >> s.repeats
               >> s.channel >> s.frequency
               >> s.flags;
            // 4 trailing pad bytes intentionally not read.
            return s;
        }

        // Locate top-level FORM 8SVX entries inside FORM ANIM and copy each
        // (including the leading "FORM"+size header, so the bytes are
        // self-contained for `musac::io_from_memory` + decoder_8svx). The IFF
        // handler API is chunk-oriented; reconstructing nested FORM bytes
        // from chunk events is awkward, so we walk file_bytes_ directly.
        std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>
        extract_8svx_bank(std::span<const std::uint8_t> file_bytes) {
            std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> out;
            if (file_bytes.size() < 12u) return out;

            // Outer FORM ANIM header at offset 0..11.
            if (std::memcmp(file_bytes.data(), "FORM", 4) != 0 ||
                std::memcmp(file_bytes.data() + 8, "ANIM", 4) != 0) {
                return out;
            }
            const std::uint32_t outer_size =
                bytes::read_u32be(file_bytes.data() + 4);
            const std::size_t outer_end =
                std::min<std::size_t>(file_bytes.size(), 8u + outer_size);

            // Walk children of FORM ANIM (start right after "ANIM" type tag).
            std::size_t pos = 12u;
            while (pos + 8u <= outer_end) {
                const auto* p = file_bytes.data() + pos;
                const std::uint32_t sz = bytes::read_u32be(p + 4);
                const std::size_t chunk_end = pos + 8u + sz;
                if (chunk_end > outer_end) break;
                if (std::memcmp(p, "FORM", 4) == 0 && sz >= 4u) {
                    if (std::memcmp(p + 8, "8SVX", 4) == 0) {
                        // Capture FORM header + body verbatim.
                        auto buf = std::make_shared<std::vector<std::uint8_t>>(
                            file_bytes.data() + pos,
                            file_bytes.data() + chunk_end);
                        out.push_back(std::move(buf));
                    } else if (std::memcmp(p + 8, "ILBM", 4) == 0) {
                        // 8SVX bank only ever appears before the first ILBM
                        // per the ANIM+SLA convention; stop scanning.
                        break;
                    }
                }
                pos = chunk_end;
                if (sz & 1u) ++pos;  // IFF padding
            }
            return out;
        }

        // ----- Audio: PCM bytes wrapped as a real musac decoder ---------------
        //
        // Architecture: the codec at open() concatenates SBDY frames into one
        // `int8_t` buffer (held via shared_ptr so the io_stream can outlive the
        // codec if needed). `take_audio_track()` builds a real `musac::decoder`
        // that reads from a `musac::io_stream` over those bytes and returns a
        // ready-to-play `musac::audio_source`. The audio thread never touches
        // codec state; the player is responsible for syncing audio playback
        // position when it issues a video seek (calls
        // `audio_stream::seek_to_time` directly).

        // Storage that backs the io_stream. Held by shared_ptr so the bytes
        // outlive any caller that holds onto the audio_source.
        using pcm_buffer = std::shared_ptr<const std::vector<std::int8_t>>;

        // Minimal decoder for raw signed 8-bit interleaved PCM. Reads bytes
        // from its io_stream, converts to float in [-1, 1]. No header parsing —
        // the rate/channels are baked in at construction time.
        class pcm_audio_decoder final : public musac::decoder {
            public:
                pcm_audio_decoder(musac::sample_rate_t rate,
                                  musac::channels_t   channels,
                                  pcm_buffer          owned_bytes) noexcept
                    : rate_(rate),
                      channels_(channels),
                      owned_bytes_(std::move(owned_bytes)) {}

                [[nodiscard]] const char* get_name() const override {
                    return "Amiga raw 8-bit signed PCM";
                }

                void open(musac::io_stream* stream) override {
                    stream_ = stream;
                    set_is_open(stream_ != nullptr);
                }

                [[nodiscard]] musac::channels_t get_channels() const override {
                    return channels_;
                }
                [[nodiscard]] musac::sample_rate_t get_rate() const override {
                    return rate_;
                }

                bool rewind() override {
                    return stream_ && stream_->seek(0, musac::seek_origin::set) >= 0;
                }

                [[nodiscard]] std::chrono::microseconds duration() const override {
                    if (!rate_ || !channels_ || !owned_bytes_) {
                        return std::chrono::microseconds{0};
                    }
                    const auto sample_frames =
                        static_cast<std::int64_t>(owned_bytes_->size() / channels_);
                    return std::chrono::microseconds{
                        sample_frames * 1'000'000LL /
                        static_cast<std::int64_t>(rate_)};
                }

                bool seek_to_time(std::chrono::microseconds pos) override {
                    if (!stream_ || !rate_ || !channels_) return false;
                    const std::int64_t sample_frames =
                        std::max<std::int64_t>(0, pos.count()) *
                        static_cast<std::int64_t>(rate_) / 1'000'000LL;
                    // 1 byte per sample for 8-bit; channels samples per frame.
                    const std::int64_t byte_offset =
                        sample_frames * static_cast<std::int64_t>(channels_);
                    return stream_->seek(byte_offset, musac::seek_origin::set) >= 0;
                }

            protected:
                std::size_t do_decode(float* buf, std::size_t len,
                                      bool& call_again) override {
                    if (!stream_ || !channels_) {
                        call_again = false;
                        return 0;
                    }
                    // 1 byte per int8 sample → request `len` bytes for `len`
                    // float samples. Read may short-return at EOF.
                    std::vector<std::int8_t> tmp(len);
                    const auto got = stream_->read(tmp.data(), tmp.size());
                    for (std::size_t i = 0; i < got; ++i) {
                        buf[i] = static_cast<float>(tmp[i]) / 128.0f;
                    }
                    call_again = got == tmp.size();
                    return got;
                }

            private:
                musac::sample_rate_t rate_     = 0;
                musac::channels_t    channels_ = 0;
                pcm_buffer           owned_bytes_;
                musac::io_stream*    stream_   = nullptr;
        };

        [[nodiscard]] bool read_full_file(musac::io_stream* s,
                                          std::vector<std::uint8_t>& out) {
            const auto sz = s->get_size();
            if (sz < 0) return false;
            out.resize(static_cast<std::size_t>(sz));
            if (s->seek(0, musac::seek_origin::set) < 0) return false;
            return s->read(out.data(), out.size()) == out.size();
        }

        // Works for both iff::chunk_event and iff::chunk_iterator::chunk_info,
        // which share `reader` field shape but have distinct types.
        template <typename T>
        [[nodiscard]] std::vector<std::uint8_t>
        read_chunk_bytes(T const& e) {
            if (!e.reader) return {};
            auto raw = e.reader->read_all();
            std::vector<std::uint8_t> out(raw.size());
            std::memcpy(out.data(), raw.data(), raw.size());
            return out;
        }

        class amiga_anim_decoder_impl final : public anim_decoder {
            public:
                using anim_decoder::open;

                [[nodiscard]] std::string_view name() const noexcept override {
                    return amiga_anim_decoder::codec_name;
                }
                [[nodiscard]] std::span<const std::string_view> extensions() const noexcept override {
                    return {kAnimExtensions.data(), kAnimExtensions.size()};
                }

                [[nodiscard]] bool sniff(musac::io_stream* s) const override {
                    if (!s) return false;
                    const auto pos = s->tell();
                    std::uint8_t buf[12] = {};
                    const auto n = s->read(buf, sizeof(buf));
                    s->seek(pos, musac::seek_origin::set);
                    if (n < 12) return false;
                    return buf[0]=='F' && buf[1]=='O' && buf[2]=='R' && buf[3]=='M' &&
                           buf[8]=='A' && buf[9]=='N' && buf[10]=='I' && buf[11]=='M';
                }

                [[nodiscard]] result open(musac::io_stream* s,
                                          const decode_options& opts) override {
                    if (!s) return make_unexpected<error_type>("anim: null stream");
                    stream_ = s;

                    // Slurp the file — corpus tops out at a few tens of MB,
                    // and we need random access via libiff's istream parser.
                    if (!read_full_file(s, file_bytes_)) {
                        return make_unexpected<error_type>("anim: cannot read file");
                    }

                    // Extract the ANIM+SLA 8SVX sound bank (if any) before the
                    // IFF walk so per-frame SCTL handlers can resolve slot
                    // numbers to byte buffers in one pass.
                    sound_bank_ = extract_8svx_bank(
                        {file_bytes_.data(), file_bytes_.size()});

                    if (auto r = parse_iff_tree(); !r) {
                        return make_unexpected<error_type>(r.error());
                    }
                    if (frames_.empty() || !frames_.front().bmhd.has_value()) {
                        return make_unexpected<error_type>("anim: no frames or missing BMHD");
                    }

                    const auto& head = *frames_.front().bmhd;
                    if (head.compress != anim::compression::byte_run1 &&
                        head.compress != anim::compression::none) {
                        return make_unexpected<error_type>("anim: unsupported BODY compression");
                    }
                    if (head.planes == 0 || head.planes > 8) {
                        return make_unexpected<error_type>("anim: unsupported plane count");
                    }
                    if (head.width  > opts.max_width ||
                        head.height > opts.max_height) {
                        return make_unexpected<error_type>("anim: dimensions exceed configured limit");
                    }

                    // Inherit CAMG from frame 0 if present.
                    camg_ = frames_.front().camg.value_or(0u);
                    cmap_rgb_ = frames_.front().cmap_rgb.value_or(std::vector<std::uint8_t>{});

                    // Set anim_info.
                    width_   = head.width;
                    height_  = head.height;
                    planes_  = head.planes;
                    bpr_     = (static_cast<std::size_t>(width_) + 7u) / 8u;

                    is_ham_ = (camg_ & anim::kCamgHam) != 0u && (planes_ == 6 || planes_ == 8);

                    info_.width        = width_;
                    info_.height       = height_;
                    info_.format       = is_ham_ ? pixel_format::rgb888
                                                  : pixel_format::indexed8;
                    info_.frame_count  = static_cast<unsigned int>(frames_.size());
                    // ANHD time fields use jiffies (1/60 s on Amiga). Use the first
                    // delta-frame's reltime as a default frame period if available;
                    // otherwise pick a sensible 60 Hz default.
                    std::uint32_t jiffies = 0;
                    for (const auto& f : frames_) {
                        if (f.anhd && f.anhd->reltime > 0) {
                            jiffies = f.anhd->reltime;
                            break;
                        }
                    }
                    if (jiffies == 0) jiffies = 1; // safe default ~16.7ms
                    info_.frame_period = std::chrono::microseconds(
                        static_cast<std::int64_t>(jiffies) * 1'000'000LL / 60LL);
                    info_.duration = info_.frame_period * info_.frame_count;

                    // Allocate persistent planar buffers (double-buffered for op 5).
                    plane_stride_ = bpr_ * height_;
                    fb_a_.assign(static_cast<std::size_t>(planes_) * plane_stride_, 0);
                    fb_b_.assign(static_cast<std::size_t>(planes_) * plane_stride_, 0);
                    fb_chunky_.assign(static_cast<std::size_t>(width_) * height_, 0);
                    if (is_ham_) {
                        fb_rgb_.assign(static_cast<std::size_t>(width_) * height_ * 3u, 0);
                    }
                    palette_.fill(0);

                    cursor_ = 0;
                    last_buffer_ = 0;

                    // ---- Audio: build pre-loaded PCM buffer if SXHD was found.
                    if (auto r = build_audio_track(); !r) {
                        return make_unexpected<error_type>(r.error());
                    }

                    return {};
                }

                [[nodiscard]] const anim_info& info() const noexcept override { return info_; }

                [[nodiscard]] frame_result decode_frame(onyx_image::surface& out) override {
                    auto fi = advance_state();
                    if (!fi) return fi;
                    if (auto r = present(out); !r) {
                        return make_unexpected<error_type>(r.error());
                    }
                    return fi;
                }

                [[nodiscard]] bool eof() const noexcept override {
                    return cursor_ >= frames_.size();
                }

                bool rewind() override { return seek_to_frame(0); }

                bool seek_to_frame(unsigned int idx) override {
                    if (frames_.empty()) return false;
                    if (idx > frames_.size()) return false;
                    if (idx < cursor_) {
                        cursor_ = 0;
                        std::fill(fb_a_.begin(), fb_a_.end(), std::uint8_t{0});
                        std::fill(fb_b_.begin(), fb_b_.end(), std::uint8_t{0});
                        std::fill(fb_chunky_.begin(), fb_chunky_.end(), std::uint8_t{0});
                        palette_.fill(0);
                        last_buffer_ = 0;
                    }
                    // Drop any pending events from the prior frame; advancing
                    // through frames will repopulate as decode_frame replays.
                    last_decoded_audio_events_ = nullptr;
                    while (cursor_ < idx) {
                        if (!advance_state()) return false;
                    }
                    // Audio is the player's responsibility to resync — call
                    // `audio_stream::seek_to_time(frame_period * idx)` after
                    // this returns.
                    return true;
                }

                bool seek_to_time(std::chrono::microseconds pts) override {
                    if (info_.frame_period.count() <= 0) return false;
                    if (frames_.empty()) return false;
                    std::int64_t f = (pts.count() < 0)
                        ? 0
                        : pts.count() / info_.frame_period.count();
                    if (f < 0) f = 0;
                    const auto last = static_cast<std::int64_t>(frames_.size());
                    if (f > last) f = last;
                    return seek_to_frame(static_cast<unsigned int>(f));
                }

                [[nodiscard]] unsigned int audio_track_count() const noexcept override {
                    return audio_pcm_ ? 1u : 0u;
                }

                [[nodiscard]] std::unique_ptr<musac::audio_source>
                take_audio_track(unsigned int index) override {
                    // Each track may only be taken once (decoder.hh contract).
                    if (!audio_pcm_ || index != 0u || audio_taken_) return nullptr;
                    audio_taken_ = true;
                    auto io = musac::io_from_memory(audio_pcm_->data(),
                                                    audio_pcm_->size());
                    auto dec = std::make_unique<pcm_audio_decoder>(
                        audio_rate_, audio_channels_, audio_pcm_);
                    return std::make_unique<musac::audio_source>(
                        std::move(dec), std::move(io));
                }

                [[nodiscard]] std::span<const audio_event>
                pending_audio_events() const noexcept override {
                    if (!last_decoded_audio_events_) return {};
                    return {last_decoded_audio_events_->data(),
                            last_decoded_audio_events_->size()};
                }

            private:
                // ----- IFF tree walk: populate frames_ and palette/camg -----

                [[nodiscard]] result parse_iff_tree() {
                    std::string s(reinterpret_cast<const char*>(file_bytes_.data()),
                                  file_bytes_.size());
                    std::istringstream stream(s, std::ios::binary);

                    frame_record current{};
                    std::optional<error_type> first_error;
                    auto remember_error = [&](error_type msg) {
                        if (!first_error) first_error = std::move(msg);
                    };

                    auto finalize_current = [&]() {
                        if (!current.body.empty() || !current.dlta.empty() ||
                            !current.sbdy.empty() ||
                            current.bmhd || current.anhd ||
                            current.cmap_rgb || current.camg) {
                            frames_.push_back(std::move(current));
                        }
                        current = frame_record{};
                    };

                    iff::handler_registry handlers;

                    // Each frame starts with either BMHD (frame 0) or ANHD (frames 1+).
                    // When we see one of those at the *begin* event for an already
                    // populated `current`, finalize and start a new record.
                    auto on_frame_starter = [&](auto parser, auto setter) {
                        return [&, parser, setter](const iff::chunk_event& e) {
                            if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                            // Heuristic: if `current` already has BODY/DLTA/SBDY,
                            // the previous frame ended; flush before starting fresh.
                            if (!current.body.empty() || !current.dlta.empty() ||
                                !current.sbdy.empty()) {
                                finalize_current();
                            }
                            auto bytes = read_chunk_bytes(e);
                            auto v = parser({bytes.data(), bytes.size()});
                            if (!v) { remember_error(v.error()); return; }
                            setter(*v);
                        };
                    };

                    handlers.on_chunk_in_form(kIlbmCC, kBmhdCC,
                        on_frame_starter(&anim::parse_bmhd,
                                         [&](anim::bmhd v) { current.bmhd = v; }));
                    handlers.on_chunk_in_form(kIlbmCC, kAnhdCC,
                        on_frame_starter(&anim::parse_anhd,
                                         [&](anim::anhd v) { current.anhd = v; }));

                    handlers.on_chunk_in_form(kIlbmCC, kCmapCC,
                        [&](const iff::chunk_event& e) {
                            if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                            auto bytes = read_chunk_bytes(e);
                            auto v = anim::parse_cmap({bytes.data(), bytes.size()});
                            if (!v) { remember_error(v.error()); return; }
                            current.cmap_rgb = std::move(*v);
                        });
                    handlers.on_chunk_in_form(kIlbmCC, kCamgCC,
                        [&](const iff::chunk_event& e) {
                            if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                            auto bytes = read_chunk_bytes(e);
                            auto v = anim::parse_camg({bytes.data(), bytes.size()});
                            if (!v) { remember_error(v.error()); return; }
                            current.camg = *v;
                        });
                    handlers.on_chunk_in_form(kIlbmCC, kBodyCC,
                        [&](const iff::chunk_event& e) {
                            if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                            current.body = read_chunk_bytes(e);
                        });
                    handlers.on_chunk_in_form(kIlbmCC, kDltaCC,
                        [&](const iff::chunk_event& e) {
                            if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                            current.dlta = read_chunk_bytes(e);
                        });
                    handlers.on_chunk_in_form(kIlbmCC, kSxhdCC,
                        [&](const iff::chunk_event& e) {
                            if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                            // SXHD only ever appears in the keyframe ILBM. The
                            // last one wins if (improbably) more than one is
                            // emitted.
                            auto bytes = read_chunk_bytes(e);
                            auto v = anim::parse_sxhd({bytes.data(), bytes.size()});
                            if (!v) { remember_error(v.error()); return; }
                            sxhd_ = *v;
                        });
                    handlers.on_chunk_in_form(kIlbmCC, kSbdyCC,
                        [&](const iff::chunk_event& e) {
                            if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                            // Multiple SBDYs per frame are concatenated, per the
                            // AnimFX spec ("if a frame owns two SBDYs, replay
                            // the second on the other channels").
                            auto bytes = read_chunk_bytes(e);
                            if (current.sbdy.empty()) {
                                current.sbdy = std::move(bytes);
                            } else {
                                const std::size_t old_n = current.sbdy.size();
                                current.sbdy.resize(old_n + bytes.size());
                                std::memcpy(current.sbdy.data() + old_n,
                                            bytes.data(), bytes.size());
                            }
                        });
                    handlers.on_chunk_in_form(kIlbmCC, kSctlCC,
                        [&](const iff::chunk_event& e) {
                            if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                            // The 8SVX bank is extracted before parse_iff_tree
                            // runs, so we can resolve `sound` (1-indexed) here.
                            auto bytes = read_chunk_bytes(e);
                            const auto sc = parse_sctl({bytes.data(), bytes.size()});
                            if (sc.sound == 0u) return;
                            const std::size_t slot = static_cast<std::size_t>(sc.sound) - 1u;
                            if (slot >= sound_bank_.size()) return;
                            audio_event ev{};
                            ev.sound_bytes   = sound_bank_[slot];
                            ev.volume        = std::clamp(
                                static_cast<float>(sc.volume) / 64.0f, 0.0f, 1.0f);
                            ev.freq_override = sc.frequency;
                            // SCTL convention: 0 means "loop forever" — pass it
                            // through unchanged. audio_event documents the same
                            // convention so the player can decide what to do.
                            ev.repeats       = sc.repeats;
                            ev.channel_mask  = sc.channel;
                            current.audio_events.push_back(std::move(ev));
                        });

                    try {
                        iff::parse(stream, handlers);
                    } catch (const std::exception& ex) {
                        return make_unexpected<error_type>(
                            std::string("anim: IFF parse failed: ") + ex.what());
                    }
                    if (first_error) {
                        return make_unexpected<error_type>(*first_error);
                    }
                    // Flush the last frame.
                    finalize_current();
                    return {};
                }

                // ----- frame decode -----

                [[nodiscard]] frame_result advance_state() {
                    if (cursor_ >= frames_.size()) {
                        return make_unexpected<error_type>("anim: end of stream");
                    }
                    const auto& f = frames_[cursor_];

                    // Pick up palette updates (frame 0 always has CMAP; later frames may).
                    if (f.cmap_rgb && !f.cmap_rgb->empty()) {
                        cmap_rgb_ = *f.cmap_rgb;
                    }
                    if (f.camg) camg_ = *f.camg;

                    // Double-buffered op-5 (matching ffmpeg's iff.c):
                    //   - Keyframe: decompress BODY into fb_a_, mirror into fb_b_.
                    //   - Delta frame: XOR delta into fb_a_ (which currently
                    //     holds frame N-2), the result IS frame N. Then swap
                    //     fb_a_ ↔ fb_b_ so the next delta XORs against
                    //     frame N-1 (now in fb_a_) to make frame N+1.
                    //
                    // The frame to emit is fb_a_ AFTER the XOR but BEFORE the
                    // swap; for the keyframe path, that's just fb_a_.
                    if (!f.body.empty()) {
                        // For a keyframe, unpack BODY into fb_a_ then mirror
                        // to fb_b_ so the first delta finds the keyframe in
                        // either buffer (matching ffmpeg's
                        // `memcpy(video[1], video[0])` after the keyframe).
                        const std::size_t expected =
                            static_cast<std::size_t>(planes_) * bpr_ * height_;
                        const bool body_packed = f.bmhd && f.bmhd->compress ==
                                                 anim::compression::byte_run1;
                        if (body_packed) {
                            if (auto r = anim::unpack_byterun1(
                                    {f.body.data(), f.body.size()},
                                    fb_a_.data(), expected); !r) {
                                return make_unexpected<error_type>(r.error());
                            }
                        } else {
                            const std::size_t n = f.body.size() < expected
                                ? f.body.size() : expected;
                            std::memcpy(fb_a_.data(), f.body.data(), n);
                            if (n < expected) {
                                std::memset(fb_a_.data() + n, 0, expected - n);
                            }
                        }
                        std::memcpy(fb_b_.data(), fb_a_.data(), expected);
                    } else if (!f.dlta.empty()) {
                        const std::uint8_t   op       = f.anhd ? f.anhd->operation : 0u;
                        const std::uint32_t  bits     = f.anhd ? f.anhd->bits      : 0u;
                        // ANHD `bits` bit 0 toggles long/short mode for ops 7/8.
                        const bool           is_short = !(bits & 0x1u);
                        const std::span<const std::uint8_t> dlta_span{
                            f.dlta.data(), f.dlta.size()};
                        const std::size_t fb_total =
                            static_cast<std::size_t>(planes_) * bpr_ * height_;

                        anim::result r;
                        switch (op) {
                            case 3:
                                r = anim::apply_dlta_op3(dlta_span, fb_a_.data(),
                                                         width_, planes_, fb_total);
                                break;
                            case 5:
                                r = anim::apply_dlta_op5(dlta_span, fb_a_.data(),
                                                         bpr_, planes_, height_);
                                break;
                            case 7:
                                r = is_short
                                    ? anim::apply_dlta_op7_short(
                                          dlta_span, fb_a_.data(), width_, planes_, fb_total)
                                    : anim::apply_dlta_op7_long(
                                          dlta_span, fb_a_.data(), width_, planes_, fb_total);
                                break;
                            case 8:
                                r = is_short
                                    ? anim::apply_dlta_op8_short(
                                          dlta_span, fb_a_.data(), width_, planes_, fb_total)
                                    : anim::apply_dlta_op8_long(
                                          dlta_span, fb_a_.data(), width_, planes_, fb_total);
                                break;
                            case 0x4A: // 'J' = decimal 74
                                r = anim::apply_dlta_opj(dlta_span, fb_a_.data(),
                                                         width_, height_, planes_, fb_total);
                                break;
                            case 0x64: // 'd' = decimal 100 (Scala ANIM32)
                                if (bits & 0x40u) {
                                    return make_unexpected<error_type>(
                                        "anim: op 100 with interlace flag not supported");
                                }
                                r = anim::apply_dlta_op_d(dlta_span, fb_a_.data(),
                                                          width_, planes_, fb_total);
                                break;
                            case 0x65: // 'e' = decimal 101 (Scala ANIM16)
                                if (bits & 0x40u) {
                                    return make_unexpected<error_type>(
                                        "anim: op 101 with interlace flag not supported");
                                }
                                r = anim::apply_dlta_op_e(dlta_span, fb_a_.data(),
                                                          width_, planes_, fb_total);
                                break;
                            case 0x6C: // 'l' = decimal 108
                                // Same `is_short` convention as ops 7/8: ANHD
                                // bits & 1 == 0 → short/vertical (full-pitch
                                // stride between writes).
                                r = anim::apply_dlta_op_l(dlta_span, fb_a_.data(),
                                                          width_, planes_, is_short,
                                                          fb_total);
                                break;
                            default:
                                return make_unexpected<error_type>(
                                    "anim: unsupported ANIM op " + std::to_string(op));
                        }
                        if (!r) return make_unexpected<error_type>(r.error());
                    }
                    // (else: empty frame — no change.)

                    anim::planar_interleaved_to_chunky(
                        fb_a_.data(), bpr_, planes_,
                        fb_chunky_.data(), width_, width_, height_);

                    if (!f.dlta.empty()) {
                        // ANHD `bits == 2` is the brush flag — ffmpeg's
                        // libavcodec/iff.c skips the double-buffer swap for
                        // these frames so the next delta lands on the
                        // just-rendered frame instead of the 2-frames-old one.
                        const std::uint32_t bits =
                            f.anhd ? f.anhd->bits : 0u;
                        if (bits != 2u) {
                            std::swap(fb_a_, fb_b_);
                        }
                    }

                    // Update palette if we have a CMAP (planar+EHB doubling).
                    update_palette_buffer();

                    frame_info fi{};
                    fi.index    = static_cast<unsigned int>(cursor_);
                    fi.pts      = info_.frame_period * static_cast<std::int64_t>(cursor_);
                    fi.duration = (f.anhd && f.anhd->reltime > 0)
                        ? std::chrono::microseconds(
                              static_cast<std::int64_t>(f.anhd->reltime) * 1'000'000LL / 60LL)
                        : info_.frame_period;
                    fi.palette_changed = static_cast<bool>(f.cmap_rgb);
                    fi.keyframe        = !f.body.empty();
                    // Stage the just-decoded frame's audio events for the
                    // caller to drain via pending_audio_events(). Stored as a
                    // pointer-into-frames_ avoids a copy; the span is invalid
                    // after the next decode_frame()/seek_*() call as documented.
                    last_decoded_audio_events_ = &f.audio_events;
                    ++cursor_;
                    return fi;
                }

                void update_palette_buffer() noexcept {
                    palette_.fill(0);
                    const std::size_t entries = cmap_rgb_.size() / 3u;
                    const std::size_t copy_n = (entries < 256u) ? entries : 256u;
                    if (copy_n > 0) {
                        std::memcpy(palette_.data(), cmap_rgb_.data(), copy_n * 3u);
                    }
                    // If EHB is set and we have ≤32 entries, populate 32..63 with
                    // half-brightness of 0..31.
                    if ((camg_ & anim::kCamgEhb) != 0u && entries <= 32u) {
                        for (std::size_t i = 0; i < 32; ++i) {
                            palette_[(32 + i) * 3 + 0] = static_cast<std::uint8_t>(palette_[i * 3 + 0] >> 1);
                            palette_[(32 + i) * 3 + 1] = static_cast<std::uint8_t>(palette_[i * 3 + 1] >> 1);
                            palette_[(32 + i) * 3 + 2] = static_cast<std::uint8_t>(palette_[i * 3 + 2] >> 1);
                        }
                    }
                }

                // Hold-And-Modify renderer. Each chunky-pixel byte's top two
                // bits select the operation: 00=palette lookup, 01=modify B,
                // 10=modify R, 11=modify G. The kept R/G/B carry forward
                // from the previous pixel within the row; rows reset to the
                // first palette entry, matching ffmpeg's `delta = pal[1]`.
                void render_ham_to_rgb() noexcept {
                    const unsigned int hold_bits =
                        (planes_ == 8) ? 6u : 4u;
                    const std::uint8_t mode_shift =
                        static_cast<std::uint8_t>(hold_bits);
                    const std::uint8_t val_mask =
                        static_cast<std::uint8_t>((1u << hold_bits) - 1u);
                    const unsigned int sl = 8u - hold_bits;

                    for (unsigned int y = 0; y < height_; ++y) {
                        const std::uint8_t* src =
                            fb_chunky_.data() +
                            static_cast<std::size_t>(y) * width_;
                        std::uint8_t* dst =
                            fb_rgb_.data() +
                            static_cast<std::size_t>(y) * width_ * 3u;
                        std::uint8_t r = palette_[0];
                        std::uint8_t g = palette_[1];
                        std::uint8_t b = palette_[2];
                        for (unsigned int x = 0; x < width_; ++x) {
                            const std::uint8_t v    = src[x];
                            const std::uint8_t mode = static_cast<std::uint8_t>(
                                v >> mode_shift);
                            const std::uint8_t val  = static_cast<std::uint8_t>(
                                v & val_mask);
                            if (mode == 0) {
                                const std::size_t pi =
                                    static_cast<std::size_t>(val) * 3u;
                                r = palette_[pi + 0];
                                g = palette_[pi + 1];
                                b = palette_[pi + 2];
                            } else {
                                // Replicate the `hold_bits`-bit modify value
                                // into 8 bits (matching ffmpeg's
                                // `tmp = val << (8-ham); tmp |= tmp >> ham;`).
                                const std::uint8_t shifted =
                                    static_cast<std::uint8_t>(val << sl);
                                const std::uint8_t rep =
                                    static_cast<std::uint8_t>(
                                        shifted | (shifted >> hold_bits));
                                if      (mode == 1) b = rep;
                                else if (mode == 2) r = rep;
                                else                g = rep;
                            }
                            dst[x * 3 + 0] = r;
                            dst[x * 3 + 1] = g;
                            dst[x * 3 + 2] = b;
                        }
                    }
                }

                [[nodiscard]] result present(onyx_image::surface& out) {
                    if (is_ham_) {
                        render_ham_to_rgb();
                        if (!out.set_size(static_cast<int>(width_),
                                          static_cast<int>(height_),
                                          pixel_format::rgb888)) {
                            return make_unexpected<error_type>("anim: surface set_size failed");
                        }
                        for (unsigned int y = 0; y < height_; ++y) {
                            out.write_pixels(
                                0,
                                static_cast<int>(y),
                                static_cast<int>(width_ * 3u),  // bytes, not pixels
                                fb_rgb_.data() +
                                    static_cast<std::size_t>(y) * width_ * 3u);
                        }
                        return {};
                    }
                    if (!out.set_size(static_cast<int>(width_),
                                      static_cast<int>(height_),
                                      pixel_format::indexed8)) {
                        return make_unexpected<error_type>("anim: surface set_size failed");
                    }
                    out.set_palette_size(256);
                    out.write_palette(0, std::span<const std::uint8_t>{
                        palette_.data(), palette_.size()});
                    for (unsigned int y = 0; y < height_; ++y) {
                        out.write_pixels(
                            0,
                            static_cast<int>(y),
                            static_cast<int>(width_),
                            fb_chunky_.data() + static_cast<std::size_t>(y) * width_);
                    }
                    return {};
                }

                // ----- Audio: pre-load all SBDY frames into one PCM buffer -----
                //
                // Stereo SBDY layout (per AnimFX): each chunk holds the L block
                // followed by the R block. We interleave to LRLR... at load
                // time so the audio decoder needs no de-interleaving. The
                // assembled bytes are owned via a shared_ptr so they outlive
                // the codec if a caller keeps the audio_source alive past
                // file close.
                [[nodiscard]] result build_audio_track() {
                    if (!sxhd_) return {};

                    if (sxhd_->compression != 0u) {
                        return make_unexpected<error_type>(
                            "anim: SXHD compression != 0 not supported");
                    }
                    if (sxhd_->sample_depth != 8u) {
                        return make_unexpected<error_type>(
                            "anim: SXHD sample depth other than 8 not supported");
                    }
                    if (sxhd_->used_mode != 1u && sxhd_->used_mode != 2u) {
                        return make_unexpected<error_type>(
                            "anim: SXHD used_mode must be 1 (mono) or 2 (stereo)");
                    }
                    if (sxhd_->play_freq == 0u) {
                        return make_unexpected<error_type>(
                            "anim: SXHD play_freq is zero");
                    }

                    const musac::channels_t ch =
                        static_cast<musac::channels_t>(sxhd_->used_mode);

                    auto pcm = std::make_shared<std::vector<std::int8_t>>();
                    // Reserve roughly the right size: most files have
                    // SXHD.length samples per channel per frame.
                    pcm->reserve(frames_.size() *
                                 static_cast<std::size_t>(sxhd_->length) * ch);

                    for (const auto& f : frames_) {
                        if (f.sbdy.empty()) continue;
                        if (ch == 1) {
                            // Mono: append the SBDY bytes verbatim as int8.
                            const auto* src = reinterpret_cast<const std::int8_t*>(
                                f.sbdy.data());
                            pcm->insert(pcm->end(), src, src + f.sbdy.size());
                        } else {
                            // Stereo: SBDY is `LL...LL RR...RR`; interleave.
                            // Both halves must be equal in size; if the chunk
                            // is odd-sized, drop the trailing byte.
                            const std::size_t half = f.sbdy.size() / 2u;
                            const auto* L = reinterpret_cast<const std::int8_t*>(
                                f.sbdy.data());
                            const auto* R = L + half;
                            const std::size_t base = pcm->size();
                            pcm->resize(base + half * 2u);
                            for (std::size_t i = 0; i < half; ++i) {
                                (*pcm)[base + i * 2u + 0u] = L[i];
                                (*pcm)[base + i * 2u + 1u] = R[i];
                            }
                        }
                    }

                    audio_pcm_      = std::move(pcm);
                    audio_rate_     = static_cast<musac::sample_rate_t>(sxhd_->play_freq);
                    audio_channels_ = ch;
                    info_.audio_track_count = 1u;
                    info_.audio_rate        = audio_rate_;
                    info_.audio_channels    = audio_channels_;
                    return {};
                }

                musac::io_stream*             stream_ = nullptr;
                std::vector<std::uint8_t>     file_bytes_;
                std::vector<frame_record>     frames_;
                std::vector<std::uint8_t>     cmap_rgb_;
                std::uint32_t                 camg_ = 0;
                std::optional<anim::sxhd>     sxhd_;
                pcm_buffer                    audio_pcm_;
                musac::sample_rate_t          audio_rate_     = 0;
                musac::channels_t             audio_channels_ = 0;
                bool                          audio_taken_ = false;

                // ANIM+SLA: top-level 8SVX FORMs (sound bank). Each entry is a
                // shared_ptr so per-frame audio_event entries can reference it
                // cheaply and stay valid past decode_frame()/seek_*().
                std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>
                                              sound_bank_;
                // Pointer to the just-decoded frame's audio_events. Reset
                // (nulled) on seek; invalidated when frames_ is rebuilt.
                const std::vector<audio_event>* last_decoded_audio_events_ = nullptr;

                anim_info                     info_{};
                unsigned int                  width_  = 0;
                unsigned int                  height_ = 0;
                unsigned int                  planes_ = 0;
                std::size_t                   bpr_    = 0;
                std::size_t                   plane_stride_ = 0;
                std::size_t                   cursor_ = 0;
                unsigned int                  last_buffer_ = 0;

                std::vector<std::uint8_t>     fb_a_;
                std::vector<std::uint8_t>     fb_b_;
                std::vector<std::uint8_t>     fb_rgb_;
                bool                          is_ham_ = false;
                std::vector<std::uint8_t>     fb_chunky_;
                std::array<std::uint8_t, 768> palette_{};
        };
    } // namespace

    std::unique_ptr<anim_decoder> amiga_anim_decoder::create() {
        return std::make_unique<amiga_anim_decoder_impl>();
    }
} // namespace onyx_anim
