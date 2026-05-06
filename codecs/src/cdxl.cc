#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <cdxl/header.hh>

#include <musac/audio_source.hh>
#include <musac/sdk/decoder.hh>
#include <musac/sdk/io_stream.hh>

#include "cdxl.hh"
#include "codec_common.hh"

namespace onyx_anim {
    namespace {
        constexpr std::array<std::string_view, 4> kCdxlExtensions = {
            ".cdxl", ".CDXL", ".xl", ".XL"
        };

        // CDXL has no native frame-period field. iffanimplay defaults to 12
        // FPS as the canonical CDTV-era rate; we follow suit and derive the
        // audio sample rate so the per-chunk audio block plays in sync.
        constexpr unsigned int kDefaultFps = 12;

        // Audio (pcm_audio_decoder), bitplanar→chunky and HAM rendering all
        // live in codec_common.hh now — see the header for design notes.
        // CDXL HAM8 uses keep-prev's-low-bits (the ffmpeg cdxl.c convention).
        using detail::pcm_buffer;
        using detail::pcm_audio_decoder;

        // ----- Decoder --------------------------------------------------------

        class cdxl_decoder_impl final : public anim_decoder {
            public:
                using anim_decoder::open;

                [[nodiscard]] std::string_view name() const noexcept override {
                    return cdxl_decoder::codec_name;
                }
                [[nodiscard]] std::span<const std::string_view>
                extensions() const noexcept override {
                    return {kCdxlExtensions.data(), kCdxlExtensions.size()};
                }

                [[nodiscard]] bool sniff(musac::io_stream* s) const override {
                    if (!s) return false;
                    const auto pos = s->tell();
                    std::uint8_t buf[cdxl::kChunkHeaderSize] = {};
                    const auto n = s->read(buf, sizeof(buf));
                    if (n < sizeof(buf)) {
                        s->seek(pos, musac::seek_origin::set);
                        return false;
                    }
                    auto h = cdxl::parse_chunk_header({buf, sizeof(buf)});
                    if (!h) {
                        s->seek(pos, musac::seek_origin::set);
                        return false;
                    }
                    // CDXL has no magic. The sniff signals we can use:
                    //   - header parses cleanly (type / info-byte enums valid)
                    //   - dimensions and plane count in plausible ranges
                    //   - the chunk holds at least a header + cmap + the
                    //     minimum bit-planar bitmap + audio (some real files
                    //     pack a few extra padding bytes per chunk)
                    //   - file size divides into chunks evenly-ish (iffanimplay
                    //     assumes constant chunk size; samples confirm this).
                    bool ok = true;
                    ok &= h->width  > 0u && h->width  <= 4096u;
                    ok &= h->height > 0u && h->height <= 4096u;
                    ok &= h->planes > 0u && h->planes <= 24u;
                    const std::size_t bitmap_min =
                        cdxl::bitplane_pitch(h->width) *
                        static_cast<std::size_t>(h->planes) *
                        static_cast<std::size_t>(h->height);
                    const std::size_t min_csize =
                        cdxl::kChunkHeaderSize + h->cmap_bytes +
                        bitmap_min + h->audio_bytes;
                    ok &= h->csize_cur >= min_csize;
                    if (ok) {
                        const auto sz = s->get_size();
                        if (sz <= 0 || h->csize_cur == 0u ||
                            static_cast<std::uint64_t>(sz) < h->csize_cur) {
                            ok = false;
                        }
                    }
                    s->seek(pos, musac::seek_origin::set);
                    return ok;
                }

                [[nodiscard]] result open(musac::io_stream* s,
                                          const decode_options& opts) override {
                    if (!s) return make_unexpected<error_type>("cdxl: null stream");
                    stream_ = s;

                    // Read the first chunk header — the rest of the file is
                    // assumed to follow the same format (iffanimplay treats
                    // mid-file format changes as warnings, but in practice
                    // every real-world CDXL is uniform).
                    if (s->seek(0, musac::seek_origin::set) < 0) {
                        return make_unexpected<error_type>("cdxl: cannot seek to start");
                    }
                    std::uint8_t hdr[cdxl::kChunkHeaderSize] = {};
                    if (s->read(hdr, sizeof(hdr)) != sizeof(hdr)) {
                        return make_unexpected<error_type>("cdxl: file too small");
                    }
                    auto first = cdxl::parse_chunk_header({hdr, sizeof(hdr)});
                    if (!first) return make_unexpected<error_type>(first.error());
                    first_ = *first;

                    // Validate format: only bit-planar with RGB / HAM6 / HAM8.
                    if (first_.por != cdxl::pixel_orientation::bit_planar) {
                        return make_unexpected<error_type>(
                            "cdxl: only bit-planar pixel orientation is supported");
                    }
                    if (first_.venc != cdxl::video_encoding::rgb &&
                        first_.venc != cdxl::video_encoding::ham) {
                        return make_unexpected<error_type>(
                            "cdxl: only RGB-indexed and HAM video encoding are supported");
                    }
                    if (first_.venc == cdxl::video_encoding::ham &&
                        first_.planes != 6u && first_.planes != 8u) {
                        return make_unexpected<error_type>(
                            "cdxl: HAM requires 6 or 8 bitplanes");
                    }
                    if (first_.planes == 0u || first_.planes > 8u) {
                        return make_unexpected<error_type>(
                            "cdxl: unsupported plane count");
                    }
                    if (first_.width  > opts.max_width ||
                        first_.height > opts.max_height) {
                        return make_unexpected<error_type>(
                            "cdxl: dimensions exceed configured limit");
                    }
                    if (first_.csize_cur < cdxl::kChunkHeaderSize) {
                        return make_unexpected<error_type>(
                            "cdxl: chunk size smaller than header");
                    }

                    // Walk the file at csize_cur strides to count frames and
                    // accumulate audio. iffanimplay assumes constant chunk
                    // size; we do too — formats that vary mid-stream are
                    // rare and would need a per-chunk index.
                    const auto file_size = s->get_size();
                    if (file_size <= 0) {
                        return make_unexpected<error_type>("cdxl: unknown file size");
                    }
                    const auto chunks =
                        static_cast<unsigned int>(file_size / first_.csize_cur);
                    if (chunks == 0u) {
                        return make_unexpected<error_type>("cdxl: zero frames");
                    }

                    const bool has_audio = first_.audio_bytes > 0u;
                    const auto channels = static_cast<musac::channels_t>(
                        first_.stereo ? 2u : 1u);

                    auto pcm = std::make_shared<std::vector<std::int8_t>>();
                    if (has_audio) {
                        pcm->reserve(static_cast<std::size_t>(first_.audio_bytes) *
                                     chunks);
                    }
                    chunk_offsets_.assign(chunks, 0);

                    for (unsigned int i = 0; i < chunks; ++i) {
                        const std::int64_t off =
                            static_cast<std::int64_t>(i) *
                            static_cast<std::int64_t>(first_.csize_cur);
                        chunk_offsets_[i] = off;
                        if (!has_audio) continue;

                        // Audio sits at the END of each chunk — anchor on
                        // csize_cur rather than the computed bitmap size,
                        // since some files (e.g. Maku.XL) pack a few padding
                        // bytes between bitmap and audio.
                        const std::int64_t audio_off = off +
                            static_cast<std::int64_t>(first_.csize_cur) -
                            static_cast<std::int64_t>(first_.audio_bytes);
                        if (s->seek(audio_off, musac::seek_origin::set) < 0) break;
                        const std::size_t want = first_.audio_bytes;
                        const std::size_t base = pcm->size();
                        pcm->resize(base + want);
                        const auto got = s->read(pcm->data() + base, want);
                        if (got != want) {
                            // Truncated tail — drop the partial bytes and stop.
                            pcm->resize(base);
                            break;
                        }
                    }

                    // Populate anim_info.
                    info_.width        = first_.width;
                    info_.height       = first_.height;
                    info_.frame_count  = chunks;
                    info_.format       = (first_.venc == cdxl::video_encoding::ham)
                        ? pixel_format::rgb888
                        : pixel_format::indexed8;
                    info_.frame_period = std::chrono::microseconds(
                        1'000'000LL / static_cast<std::int64_t>(kDefaultFps));
                    info_.duration     = info_.frame_period * info_.frame_count;

                    if (has_audio && !pcm->empty()) {
                        // Audio rate derived from the canonical 12-FPS
                        // assumption: rate = audio_bytes/chunk × FPS / channels.
                        // Files authored for a different FPS will play with
                        // audio drift, but video still runs at 12 FPS.
                        const auto rate = static_cast<musac::sample_rate_t>(
                            (static_cast<std::uint64_t>(first_.audio_bytes) *
                             kDefaultFps) /
                            std::max<musac::channels_t>(channels, 1));
                        audio_pcm_      = std::move(pcm);
                        audio_rate_     = rate;
                        audio_channels_ = channels;
                        info_.audio_track_count = 1u;
                        info_.audio_rate        = rate;
                        info_.audio_channels    = channels;
                    }

                    // Allocate working buffers sized to the (assumed-uniform)
                    // first-chunk geometry.
                    bytes_per_plane_ = cdxl::bitplane_pitch(first_.width);
                    bitmap_size_     = bytes_per_plane_ * first_.planes * first_.height;
                    bitmap_buf_.assign(bitmap_size_, 0);
                    chunky_buf_.assign(static_cast<std::size_t>(first_.width) *
                                       first_.height, 0);
                    if (info_.format == pixel_format::rgb888) {
                        rgb_buf_.assign(static_cast<std::size_t>(first_.width) *
                                        first_.height * 3u, 0);
                    }
                    palette_888_.fill(0);

                    cursor_ = 0;
                    return {};
                }

                [[nodiscard]] const anim_info& info() const noexcept override {
                    return info_;
                }

                [[nodiscard]] frame_result decode_frame(onyx_image::surface& out) override {
                    if (cursor_ >= chunk_offsets_.size()) {
                        return make_unexpected<error_type>("cdxl: end of stream");
                    }
                    const auto idx     = cursor_;
                    const auto chunk_off = chunk_offsets_[idx];

                    if (stream_->seek(chunk_off, musac::seek_origin::set) < 0) {
                        return make_unexpected<error_type>("cdxl: seek to chunk failed");
                    }

                    std::uint8_t hdr[cdxl::kChunkHeaderSize] = {};
                    if (stream_->read(hdr, sizeof(hdr)) != sizeof(hdr)) {
                        return make_unexpected<error_type>("cdxl: chunk header read failed");
                    }
                    auto h = cdxl::parse_chunk_header({hdr, sizeof(hdr)});
                    if (!h) return make_unexpected<error_type>(h.error());
                    if (h->csize_cur != first_.csize_cur ||
                        h->width != first_.width ||
                        h->height != first_.height ||
                        h->planes != first_.planes ||
                        h->por != first_.por ||
                        h->venc != first_.venc) {
                        return make_unexpected<error_type>(
                            "cdxl: mid-stream format changes are not supported");
                    }
                    const std::size_t chunk_min =
                        cdxl::kChunkHeaderSize +
                        static_cast<std::size_t>(h->cmap_bytes) +
                        bitmap_size_ +
                        static_cast<std::size_t>(h->audio_bytes);
                    if (h->csize_cur < chunk_min) {
                        return make_unexpected<error_type>(
                            "cdxl: chunk smaller than declared payloads");
                    }

                    // Read colormap if present and convert to 8-bit RGB.
                    if (h->cmap_bytes > 0u) {
                        const std::size_t entries = h->cmap_bytes / 2u;
                        const std::size_t take    = std::min<std::size_t>(entries, 256u);
                        std::vector<std::uint8_t> cmap_raw(h->cmap_bytes);
                        if (stream_->read(cmap_raw.data(), cmap_raw.size())
                                != cmap_raw.size()) {
                            return make_unexpected<error_type>(
                                "cdxl: cmap read failed");
                        }
                        for (std::size_t i = 0; i < take; ++i) {
                            const auto entry = static_cast<std::uint16_t>(
                                (static_cast<unsigned>(cmap_raw[i * 2 + 0]) << 8) |
                                cmap_raw[i * 2 + 1]);
                            cdxl::rgb12_to_888(entry,
                                               palette_888_[i * 3 + 0],
                                               palette_888_[i * 3 + 1],
                                               palette_888_[i * 3 + 2]);
                        }
                        for (std::size_t i = take * 3u; i < palette_888_.size(); ++i) {
                            palette_888_[i] = 0;
                        }
                    } else if (h->cmap_bytes != 0u) {
                        // odd cmap size — skip via dummy read
                        std::vector<std::uint8_t> dummy(h->cmap_bytes);
                        (void)stream_->read(dummy.data(), dummy.size());
                    }

                    // Read bitmap.
                    if (stream_->read(bitmap_buf_.data(), bitmap_size_)
                            != bitmap_size_) {
                        return make_unexpected<error_type>("cdxl: bitmap read failed");
                    }

                    // Convert: BIT_PLANAR (plane-major) → chunky 8-bit.
                    detail::bitplanar_to_chunky(bitmap_buf_.data(),
                                                bytes_per_plane_,
                                                h->planes,
                                                h->width, h->height,
                                                chunky_buf_.data());

                    // Present.
                    if (info_.format == pixel_format::rgb888) {
                        if (!out.set_size(static_cast<int>(h->width),
                                          static_cast<int>(h->height),
                                          pixel_format::rgb888)) {
                            return make_unexpected<error_type>(
                                "cdxl: surface set_size failed");
                        }
                        for (unsigned int y = 0; y < h->height; ++y) {
                            const std::uint8_t* src = chunky_buf_.data() +
                                                      static_cast<std::size_t>(y) * h->width;
                            std::uint8_t* row = rgb_buf_.data() +
                                                static_cast<std::size_t>(y) * h->width * 3u;
                            detail::ham_row_to_rgb888(
                                src, h->width, h->planes,
                                palette_888_.data(),
                                /*ham8_keep_prev_low_bits=*/true,
                                row);
                            out.write_pixels(
                                0,
                                static_cast<int>(y),
                                static_cast<int>(h->width * 3u),  // bytes
                                row);
                        }
                    } else {
                        if (!out.set_size(static_cast<int>(h->width),
                                          static_cast<int>(h->height),
                                          pixel_format::indexed8)) {
                            return make_unexpected<error_type>(
                                "cdxl: surface set_size failed");
                        }
                        out.set_palette_size(256);
                        out.write_palette(0, std::span<const std::uint8_t>{
                            palette_888_.data(), palette_888_.size()});
                        for (unsigned int y = 0; y < h->height; ++y) {
                            out.write_pixels(
                                0,
                                static_cast<int>(y),
                                static_cast<int>(h->width),
                                chunky_buf_.data() +
                                    static_cast<std::size_t>(y) * h->width);
                        }
                    }

                    frame_info fi{};
                    fi.index    = static_cast<unsigned int>(idx);
                    fi.pts      = info_.frame_period * static_cast<std::int64_t>(idx);
                    fi.duration = info_.frame_period;
                    fi.keyframe = true; // every CDXL frame is independent
                    ++cursor_;
                    return fi;
                }

                [[nodiscard]] bool eof() const noexcept override {
                    return cursor_ >= chunk_offsets_.size();
                }

                bool rewind() override { return seek_to_frame(0u); }

                bool seek_to_frame(unsigned int idx) override {
                    if (chunk_offsets_.empty()) return false;
                    if (idx > chunk_offsets_.size()) return false;
                    cursor_ = idx;
                    return true;
                }

                bool seek_to_time(std::chrono::microseconds pts) override {
                    if (info_.frame_period.count() <= 0) return false;
                    if (chunk_offsets_.empty())          return false;
                    std::int64_t f = pts.count() < 0
                        ? 0
                        : pts.count() / info_.frame_period.count();
                    const auto last = static_cast<std::int64_t>(chunk_offsets_.size());
                    if (f > last) f = last;
                    return seek_to_frame(static_cast<unsigned int>(f));
                }

                [[nodiscard]] unsigned int audio_track_count() const noexcept override {
                    return audio_pcm_ ? 1u : 0u;
                }

                [[nodiscard]] std::unique_ptr<musac::audio_source>
                take_audio_track(unsigned int index,
                                 musac::io_stream** io_observer) override {
                    if (!audio_pcm_ || index != 0u || audio_taken_) {
                        if (io_observer) *io_observer = nullptr;
                        return nullptr;
                    }
                    audio_taken_ = true;
                    auto io = musac::io_from_memory(audio_pcm_->data(),
                                                    audio_pcm_->size());
                    if (io_observer) *io_observer = io.get();
                    auto dec = std::make_unique<pcm_audio_decoder>(
                        "CDXL raw 8-bit signed PCM",
                        audio_rate_, audio_channels_, audio_pcm_);
                    return std::make_unique<musac::audio_source>(
                        std::move(dec), std::move(io));
                }

            private:
                musac::io_stream*             stream_ = nullptr;
                cdxl::chunk_header            first_{};
                std::vector<std::int64_t>     chunk_offsets_;
                std::size_t                   cursor_ = 0;

                std::size_t                   bytes_per_plane_ = 0;
                std::size_t                   bitmap_size_     = 0;
                std::vector<std::uint8_t>     bitmap_buf_;
                std::vector<std::uint8_t>     chunky_buf_;
                std::vector<std::uint8_t>     rgb_buf_;
                std::array<std::uint8_t, 768> palette_888_{};

                anim_info                     info_{};

                pcm_buffer                    audio_pcm_;
                musac::sample_rate_t          audio_rate_     = 0;
                musac::channels_t             audio_channels_ = 0;
                bool                          audio_taken_    = false;
        };
    } // namespace

    std::unique_ptr<anim_decoder> cdxl_decoder::create() {
        return std::make_unique<cdxl_decoder_impl>();
    }
} // namespace onyx_anim
