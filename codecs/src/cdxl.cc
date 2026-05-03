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

namespace onyx_anim {
    namespace {
        constexpr std::array<std::string_view, 4> kCdxlExtensions = {
            ".cdxl", ".CDXL", ".xl", ".XL"
        };

        // CDXL has no native frame-period field. iffanimplay defaults to 12
        // FPS as the canonical CDTV-era rate; we follow suit and derive the
        // audio sample rate so the per-chunk audio block plays in sync.
        constexpr unsigned int kDefaultFps = 12;

        // ----- Audio: PCM bytes wrapped as a real musac decoder ---------------
        //
        // Same architecture as the AnimFX path in amiga_anim.cc: pre-load all
        // per-chunk audio bytes once at open(), wrap with io_from_memory in
        // take_audio_track(), and hand back a self-contained audio_source.
        // The pcm_audio_decoder reads bytes through the io_stream — no
        // side-channel buffer, no atomic cursor.

        using pcm_buffer = std::shared_ptr<const std::vector<std::int8_t>>;

        class pcm_audio_decoder final : public musac::decoder {
            public:
                pcm_audio_decoder(musac::sample_rate_t rate,
                                  musac::channels_t   channels,
                                  pcm_buffer          owned_bytes) noexcept
                    : rate_(rate),
                      channels_(channels),
                      owned_bytes_(std::move(owned_bytes)) {}

                [[nodiscard]] const char* get_name() const override {
                    return "CDXL raw 8-bit signed PCM";
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

        // ----- Pixel conversions ----------------------------------------------

        // Convert a CDXL BIT_PLANAR bitmap to chunky 8-bit indices.
        //
        // CDXL's BIT_PLANAR layout is **plane-major**, NOT row-interleaved
        // like ILBM: the buffer holds all rows of plane 0 contiguously, then
        // all rows of plane 1, etc. (Matches ffmpeg's bitplanar2chunky in
        // libavcodec/cdxl.c — confusingly, iffanimplay's "bitPlanarToChunky"
        // assumes the ILBM row-interleaved layout, which produces a
        // characteristic "N scattered sub-images" garbled output on real
        // CDXL files.)
        //
        // Each plane row is `bytes_per_plane_row` bytes (rounded up to a
        // 16-bit word for Amiga hardware compatibility). Pixels within a
        // byte are MSB-first.
        void bitplanar_to_chunky(const std::uint8_t* src,
                                 std::size_t         bytes_per_plane_row,
                                 unsigned int        planes,
                                 unsigned int        width,
                                 unsigned int        height,
                                 std::uint8_t*       dst) noexcept {
            std::memset(dst, 0,
                        static_cast<std::size_t>(width) *
                        static_cast<std::size_t>(height));
            for (unsigned int p = 0; p < planes; ++p) {
                for (unsigned int y = 0; y < height; ++y) {
                    const std::uint8_t* row = src +
                        (static_cast<std::size_t>(p) * height +
                         static_cast<std::size_t>(y)) * bytes_per_plane_row;
                    std::uint8_t* out_row = dst +
                        static_cast<std::size_t>(y) * width;
                    for (unsigned int x = 0; x < width; ++x) {
                        const std::uint8_t bit =
                            static_cast<std::uint8_t>(
                                (row[x >> 3u] >> (7u - (x & 0x7u))) & 1u);
                        out_row[x] = static_cast<std::uint8_t>(
                            out_row[x] | (bit << p));
                    }
                }
            }
        }

        // Render one chunky row of HAM6/HAM8 indices into 8-bit-per-channel
        // RGB. Top 2 bits of each pixel are a mode (0=hold-from-palette,
        // 1=modify-blue, 2=modify-red, 3=modify-green); the lower bits are
        // the value.
        //
        // Channel-expansion convention differs between HAM6 and HAM8 in
        // CDXL — match ffmpeg's libavcodec/cdxl.c:
        //   HAM6 (val is 4 bits): replicate → 8-bit = `val * 0x11`
        //                         (the upper nibble holds val, lower nibble
        //                         is also val; prev's bits don't survive)
        //   HAM8 (val is 6 bits): keep prev's low 2 bits → 8-bit =
        //                         `(val << 2) | (prev & 3)`
        // Note: this differs from amiga_anim's ANIM HAM8 (which uses
        // replicate). Don't unify the two without checking both
        // cross-checks.
        void ham_row_to_rgb888(const std::uint8_t* src_row,
                               unsigned int        width,
                               unsigned int        planes, // 6 or 8
                               const std::uint8_t* palette_888,
                               std::uint8_t*       dst_row) noexcept {
            const bool ham8 = (planes == 8);
            const unsigned int hold_bits = ham8 ? 6u : 4u;
            const std::uint8_t mode_shift = static_cast<std::uint8_t>(hold_bits);
            const std::uint8_t val_mask =
                static_cast<std::uint8_t>((1u << hold_bits) - 1u);
            const unsigned int sl = 8u - hold_bits;
            const std::uint8_t prev_mask =
                static_cast<std::uint8_t>((1u << sl) - 1u);  // 0x03 / 0x0F

            std::uint8_t r = palette_888[0];
            std::uint8_t g = palette_888[1];
            std::uint8_t b = palette_888[2];
            for (unsigned int x = 0; x < width; ++x) {
                const std::uint8_t v    = src_row[x];
                const std::uint8_t mode = static_cast<std::uint8_t>(v >> mode_shift);
                const std::uint8_t val  = static_cast<std::uint8_t>(v & val_mask);
                if (mode == 0) {
                    const std::size_t pi = static_cast<std::size_t>(val) * 3u;
                    r = palette_888[pi + 0];
                    g = palette_888[pi + 1];
                    b = palette_888[pi + 2];
                } else {
                    const std::uint8_t shifted = static_cast<std::uint8_t>(val << sl);
                    auto modify = [&](std::uint8_t prev) -> std::uint8_t {
                        if (ham8) return static_cast<std::uint8_t>(
                            shifted | (prev & prev_mask));
                        // HAM6: pure replicate; prev bits don't survive.
                        return static_cast<std::uint8_t>(shifted | (shifted >> hold_bits));
                    };
                    if      (mode == 1) b = modify(b);
                    else if (mode == 2) r = modify(r);
                    else                g = modify(g);
                }
                dst_row[x * 3 + 0] = r;
                dst_row[x * 3 + 1] = g;
                dst_row[x * 3 + 2] = b;
            }
        }

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
                    bitplanar_to_chunky(bitmap_buf_.data(),
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
                            ham_row_to_rgb888(src, h->width, h->planes,
                                              palette_888_.data(), row);
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
                take_audio_track(unsigned int index) override {
                    if (!audio_pcm_ || index != 0u || audio_taken_) return nullptr;
                    audio_taken_ = true;
                    auto io = musac::io_from_memory(audio_pcm_->data(),
                                                    audio_pcm_->size());
                    auto dec = std::make_unique<pcm_audio_decoder>(
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
