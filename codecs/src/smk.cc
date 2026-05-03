#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <smk/header.hh>
#include <smk/decoders.hh>

#include <musac/sdk/io_stream.hh>
#include <musac/audio_source.hh>

#include "smk.hh"
#include "codec_common.hh"

namespace onyx_anim {
    namespace {
        constexpr std::array <std::string_view, 1> kSmkExtensions = {".smk"};

        using detail::read_full_file;
        using detail::pcm_audio_decoder;
        using detail::pcm_buffer;

        // Per-frame index entry pre-computed at open() time so decode_frame
        // can advance the cursor without re-walking the file from the head.
        // `size` is already masked to drop the bottom 2 status bits;
        // `keyframe` reflects bit 0 of the original size word.
        struct frame_index_entry {
            std::size_t   offset;
            std::uint32_t size;
            std::uint8_t  flags;    // raw frame_flags[] byte
            bool          keyframe;
        };

        // ----- Decoder --------------------------------------------------------

        class smk_decoder_impl final : public anim_decoder {
            public:
                using anim_decoder::open;

                [[nodiscard]] std::string_view name() const noexcept override {
                    return smk_decoder::codec_name;
                }
                [[nodiscard]] std::span <const std::string_view>
                extensions() const noexcept override {
                    return {kSmkExtensions.data(), kSmkExtensions.size()};
                }

                [[nodiscard]] bool sniff(musac::io_stream* s) const override {
                    if (!s) return false;
                    const auto pos = s->tell();
                    std::uint8_t magic[4] = {};
                    const auto n = s->read(magic, sizeof(magic));
                    s->seek(pos, musac::seek_origin::set);
                    if (n < sizeof(magic)) return false;
                    return magic[0] == 'S' && magic[1] == 'M' && magic[2] == 'K' &&
                           (magic[3] == '2' || magic[3] == '4');
                }

                [[nodiscard]] result open(musac::io_stream* s,
                                          const decode_options& opts) override {
                    if (!s) return make_unexpected<error_type>("smk: null stream");
                    if (!read_full_file(s, file_bytes_)) {
                        return make_unexpected<error_type>("smk: cannot read file");
                    }

                    auto h = smk::parse_file_header(
                        {file_bytes_.data(), file_bytes_.size()});
                    if (!h) return make_unexpected<error_type>(h.error());
                    header_ = *h;

                    if (header_.width  > opts.max_width ||
                        header_.height > opts.max_height) {
                        return make_unexpected<error_type>(
                            "smk: dimensions exceed configured limit");
                    }

                    // Layout immediately after the 104-byte header (matches
                    // ffmpeg's smacker_read_header order, which is the
                    // authoritative reference for the tail-of-header
                    // structure):
                    //   sizes[frames]  (4 bytes each)
                    //   flags[frames]  (1 byte each)
                    //   trees blob     (trees_size bytes)
                    //   frame data     (variable, summing to (sum sizes & ~3))
                    const std::size_t sizes_off = smk::kHeaderSize;
                    const std::size_t flags_off = sizes_off + header_.frames * 4u;
                    const std::size_t trees_off = flags_off + header_.frames;
                    const std::size_t data_off  = trees_off + header_.trees_size;
                    if (trees_off + header_.trees_size > file_bytes_.size()) {
                        return make_unexpected<error_type>(
                            "smk: trees blob extends past end of file");
                    }
                    auto vt = smk::build_video_trees(
                        std::span <const std::uint8_t>{
                            file_bytes_.data() + trees_off, header_.trees_size},
                        header_);
                    if (!vt) return make_unexpected<error_type>(vt.error());
                    trees_ = std::move(*vt);

                    if (data_off > file_bytes_.size()) {
                        return make_unexpected<error_type>(
                            "smk: frame index/trees tables truncated");
                    }

                    // Some real-world files are truncated mid-stream
                    // (EARTH.SMK in our corpus declares 74 frames but the
                    // file only holds bytes for ≈30 of them). Match
                    // ffmpeg's behaviour: index as many frames as fit in
                    // the file, drop the rest silently.
                    frame_index_.reserve(header_.frames);
                    std::size_t cursor = data_off;
                    for (std::uint32_t i = 0; i < header_.frames; ++i) {
                        const std::uint8_t* p = file_bytes_.data() + sizes_off + i * 4u;
                        const std::uint32_t raw_sz =
                            static_cast <std::uint32_t>(p[0]) |
                            (static_cast <std::uint32_t>(p[1]) << 8u) |
                            (static_cast <std::uint32_t>(p[2]) << 16u) |
                            (static_cast <std::uint32_t>(p[3]) << 24u);
                        // Bottom 2 bits are status flags; the actual chunk
                        // size is `raw_sz & ~3`. Bit 0 = keyframe.
                        const std::uint32_t fsz = raw_sz & ~3u;
                        const bool key = (raw_sz & 1u) != 0u || i == 0u;
                        const std::uint8_t fflags = file_bytes_[flags_off + i];
                        if (cursor + fsz > file_bytes_.size()) {
                            break; // truncated tail — present what we have
                        }
                        frame_index_.push_back({cursor, fsz, fflags, key});
                        cursor += fsz;
                    }
                    if (frame_index_.empty()) {
                        return make_unexpected<error_type>(
                            "smk: no frames fit within file");
                    }

                    // Frame buffers — one persistent paletted8 buffer that
                    // decode_frame patches in place. Smacker is fully
                    // delta-coded across frames (SKIP block re-uses the
                    // previous-frame contents).
                    fb_chunky_.assign(
                        static_cast <std::size_t>(header_.width) * header_.height, 0);

                    // Palette starts black per ffmpeg semantics.
                    palette_768_.assign(768, 0);

                    // ----- Audio: pre-decode track 0 (only) -----------------
                    // Smacker can carry up to 7 tracks; the corpus shows at
                    // most 1 in practice. We pre-decode that track into a
                    // single PCM buffer at open() time. Tracks ≥ 1 are
                    // silently dropped — adding them is a future extension.
                    audio_pcm_int8_.reset();
                    audio_rate_ = 0;
                    audio_channels_ = 0;
                    if (header_.audio[0].present && !header_.audio[0].unsupported) {
                        if (auto r = pre_decode_audio_track0(); !r) {
                            return make_unexpected<error_type>(r.error());
                        }
                    }

                    // anim_info.
                    info_.width        = header_.width;
                    info_.height       = header_.height;
                    info_.frame_count  = static_cast <unsigned int>(frame_index_.size());
                    info_.format       = pixel_format::indexed8;
                    const auto period_us = smk::frame_period_us(header_.pts_inc);
                    info_.frame_period = std::chrono::microseconds(period_us);
                    info_.duration = info_.frame_period * info_.frame_count;
                    info_.audio_track_count = audio_pcm_int8_ ? 1u : 0u;

                    cursor_ = 0;
                    return {};
                }

                [[nodiscard]] const anim_info& info() const noexcept override {
                    return info_;
                }

                [[nodiscard]] frame_result decode_frame(onyx_image::surface& out) override {
                    if (cursor_ >= frame_index_.size()) {
                        return make_unexpected<error_type>("smk: end of stream");
                    }
                    const auto& fe = frame_index_[cursor_];
                    auto frame_bytes = std::span <const std::uint8_t>(
                        file_bytes_.data() + fe.offset, fe.size);

                    // Within the frame: optional palette delta block, then
                    // up to 7 audio chunks (one per audio mask bit), then
                    // the video bit-stream. Each audio chunk's first u32
                    // gives its own size (including those 4 bytes).
                    if (fe.flags & smk::kFrameHasPalette) {
                        auto consumed = smk::apply_palette_block(
                            frame_bytes, palette_768_.data());
                        if (!consumed) return make_unexpected<error_type>(consumed.error());
                        if (*consumed > frame_bytes.size()) {
                            return make_unexpected<error_type>(
                                "smk: palette block claims more bytes than frame holds");
                        }
                        frame_bytes = frame_bytes.subspan(*consumed);
                    }

                    // Audio chunks — we already pre-decoded the audio at
                    // open() time, so here we just skip past each chunk to
                    // reach the video bit-stream.
                    for (unsigned int i = 0; i < 7; ++i) {
                        if ((fe.flags & (1u << (i + 1u))) == 0u) continue;
                        if (frame_bytes.size() < 4u) {
                            return make_unexpected<error_type>(
                                "smk: audio chunk size truncated");
                        }
                        const std::uint32_t chunk_sz =
                            static_cast <std::uint32_t>(frame_bytes[0]) |
                            (static_cast <std::uint32_t>(frame_bytes[1]) << 8u) |
                            (static_cast <std::uint32_t>(frame_bytes[2]) << 16u) |
                            (static_cast <std::uint32_t>(frame_bytes[3]) << 24u);
                        if (chunk_sz < 4u || chunk_sz > frame_bytes.size()) {
                            return make_unexpected<error_type>(
                                "smk: audio chunk size out of range");
                        }
                        frame_bytes = frame_bytes.subspan(chunk_sz);
                    }

                    // Remaining bytes are the video bit stream.
                    if (auto r = smk::decode_video_frame(
                            trees_, frame_bytes,
                            header_.width, header_.height,
                            header_.is_smk4,
                            fb_chunky_.data()); !r) {
                        return make_unexpected<error_type>(r.error());
                    }

                    // Surface emit. Smacker is paletted8 throughout; the
                    // codec can be invoked with preferred_format=indexed8
                    // (default) and the player handles palette expansion.
                    if (!out.set_size(static_cast <int>(header_.width),
                                      static_cast <int>(header_.height),
                                      pixel_format::indexed8)) {
                        return make_unexpected<error_type>("smk: surface set_size failed");
                    }
                    out.set_palette_size(256);
                    out.write_palette(0, std::span <const std::uint8_t>{
                        palette_768_.data(), palette_768_.size()});
                    for (unsigned int y = 0; y < header_.height; ++y) {
                        out.write_pixels(0, static_cast <int>(y),
                            static_cast <int>(header_.width),
                            fb_chunky_.data() +
                                static_cast <std::size_t>(y) * header_.width);
                    }

                    frame_info fi{};
                    fi.index    = static_cast <unsigned int>(cursor_);
                    fi.pts      = info_.frame_period * static_cast <std::int64_t>(cursor_);
                    fi.duration = info_.frame_period;
                    fi.keyframe = fe.keyframe;
                    ++cursor_;
                    return fi;
                }

                [[nodiscard]] bool eof() const noexcept override {
                    return cursor_ >= frame_index_.size();
                }

                bool rewind() override { return seek_to_frame(0u); }

                bool seek_to_frame(unsigned int idx) override {
                    if (frame_index_.empty()) return false;
                    if (idx > frame_index_.size()) return false;
                    if (idx == cursor_) return true;
                    // Smacker is fully delta-coded; jumping forward needs a
                    // replay from the start (or from the previous explicit
                    // keyframe — Smacker rarely emits intra-frames mid-clip,
                    // so we always replay from 0).
                    if (idx < cursor_) {
                        cursor_ = 0;
                        std::fill(fb_chunky_.begin(), fb_chunky_.end(),
                                  std::uint8_t{0});
                        std::fill(palette_768_.begin(), palette_768_.end(),
                                  std::uint8_t{0});
                    }
                    onyx_image::memory_surface tmp;
                    while (cursor_ < idx) {
                        if (!decode_frame(tmp)) return false;
                    }
                    return true;
                }

                bool seek_to_time(std::chrono::microseconds pts) override {
                    if (info_.frame_period.count() <= 0) return false;
                    if (frame_index_.empty()) return false;
                    std::int64_t f = pts.count() < 0
                        ? 0
                        : pts.count() / info_.frame_period.count();
                    const auto last =
                        static_cast <std::int64_t>(frame_index_.size());
                    if (f > last) f = last;
                    return seek_to_frame(static_cast <unsigned int>(f));
                }

                [[nodiscard]] unsigned int audio_track_count() const noexcept override {
                    return audio_pcm_int8_ ? 1u : 0u;
                }

                [[nodiscard]] std::unique_ptr<musac::audio_source>
                take_audio_track(unsigned int index) override {
                    if (!audio_pcm_int8_ || index != 0u || audio_taken_) return nullptr;
                    audio_taken_ = true;
                    auto io = musac::io_from_memory(audio_pcm_int8_->data(),
                                                    audio_pcm_int8_->size());
                    auto dec = std::make_unique<pcm_audio_decoder>(
                        "Smacker raw 8-bit signed PCM",
                        audio_rate_, audio_channels_, audio_pcm_int8_);
                    return std::make_unique<musac::audio_source>(
                        std::move(dec), std::move(io));
                }

            private:
                // Pre-decode all audio chunks for track 0 into a single
                // contiguous int8 PCM buffer. Smacker emits uint8 unsigned
                // samples (or int16 LE for 16-bit tracks); we bias-convert
                // 8-bit samples to int8 and downconvert 16-bit samples by
                // truncating to the high byte. Quality loss for 16-bit is
                // acceptable here — the alternative is extending
                // pcm_audio_decoder for int16 paths and that's its own
                // refactor.
                [[nodiscard]] result pre_decode_audio_track0() {
                    const auto& track = header_.audio[0];
                    audio_rate_ = static_cast <musac::sample_rate_t>(track.sample_rate);
                    audio_channels_ = static_cast <musac::channels_t>(track.stereo ? 2u : 1u);

                    auto bytes = std::make_shared<std::vector <std::int8_t>>();
                    bytes->reserve(64 * 1024);

                    std::vector <std::uint8_t> chunk_out;
                    for (const auto& fe : frame_index_) {
                        // Walk into the frame skipping palette block; then
                        // for each set audio bit, decode the chunk that
                        // happens to be track-0 and accumulate.
                        auto fb = std::span <const std::uint8_t>(
                            file_bytes_.data() + fe.offset, fe.size);
                        if (fe.flags & smk::kFrameHasPalette) {
                            if (fb.empty()) continue;
                            const std::size_t total =
                                static_cast <std::size_t>(fb[0]) * 4u;
                            if (total > fb.size()) {
                                return make_unexpected<error_type>(
                                    "smk: audio walk: palette block over-sized");
                            }
                            fb = fb.subspan(total);
                        }
                        for (unsigned int i = 0; i < 7; ++i) {
                            if ((fe.flags & (1u << (i + 1u))) == 0u) continue;
                            if (fb.size() < 4u) {
                                return make_unexpected<error_type>(
                                    "smk: audio walk: chunk size truncated");
                            }
                            const std::uint32_t chunk_sz =
                                static_cast <std::uint32_t>(fb[0]) |
                                (static_cast <std::uint32_t>(fb[1]) << 8u) |
                                (static_cast <std::uint32_t>(fb[2]) << 16u) |
                                (static_cast <std::uint32_t>(fb[3]) << 24u);
                            if (chunk_sz < 4u || chunk_sz > fb.size()) {
                                return make_unexpected<error_type>(
                                    "smk: audio walk: chunk size out of range");
                            }
                            const auto chunk_payload = fb.subspan(4, chunk_sz - 4u);
                            if (i == 0u) {
                                if (auto r = smk::decode_audio_chunk(
                                        track, chunk_payload, chunk_out); !r) {
                                    return make_unexpected<error_type>(r.error());
                                }
                                append_audio_samples(*bytes, chunk_out, track);
                            }
                            fb = fb.subspan(chunk_sz);
                        }
                    }

                    if (bytes->empty()) {
                        // No data emitted (every chunk was the "no data"
                        // sentinel) — leave the decoder audio-less.
                        audio_rate_ = 0;
                        audio_channels_ = 0;
                        return {};
                    }
                    audio_pcm_int8_ = std::move(bytes);
                    return {};
                }

                static void append_audio_samples(
                    std::vector <std::int8_t>& dst,
                    const std::vector <std::uint8_t>& chunk_out,
                    const smk::audio_track_info& track) {
                    if (track.bits16) {
                        // chunk_out holds int16 LE pairs. Downconvert to
                        // int8 by taking the high byte (a sign-preserving
                        // shift).
                        const std::size_t n = chunk_out.size() / 2u;
                        dst.reserve(dst.size() + n);
                        for (std::size_t i = 0; i < n; ++i) {
                            const auto lo = chunk_out[i * 2];
                            const auto hi = chunk_out[i * 2 + 1];
                            const auto s =
                                static_cast <std::int16_t>(
                                    static_cast <std::uint16_t>(lo) |
                                    (static_cast <std::uint16_t>(hi) << 8u));
                            dst.push_back(static_cast <std::int8_t>(s >> 8));
                        }
                    } else {
                        // 8-bit unsigned (Smacker DPCM result or raw PCM_U8)
                        // → int8 signed via subtracting bias 128.
                        dst.reserve(dst.size() + chunk_out.size());
                        for (const auto u : chunk_out) {
                            dst.push_back(static_cast <std::int8_t>(
                                static_cast <int>(u) - 128));
                        }
                    }
                }

                std::vector <std::uint8_t> file_bytes_;
                smk::file_header           header_{};
                smk::video_trees           trees_{};
                std::vector <frame_index_entry> frame_index_;
                std::vector <std::uint8_t> palette_768_; // RGB×256
                std::vector <std::uint8_t> fb_chunky_;
                pcm_buffer                 audio_pcm_int8_;
                musac::sample_rate_t       audio_rate_ = 0;
                musac::channels_t          audio_channels_ = 0;
                bool                       audio_taken_ = false;

                anim_info                  info_{};
                std::size_t                cursor_ = 0;
        };
    } // namespace

    std::unique_ptr <anim_decoder> smk_decoder::create() {
        return std::make_unique <smk_decoder_impl>();
    }
} // namespace onyx_anim
