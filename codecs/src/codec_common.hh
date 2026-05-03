#pragma once

// Shared adapter-side helpers used by multiple onyx_anim codec adapters.
// Header-only (inline functions and an inline class) — depends on musac
// (io_stream + decoder) and onyx_anim's audio_event type, so the consumers
// have to be linking those already. The byte-decoder primitives in lib_*
// stay separate from this; this file is for the .cc adapter glue.
//
// Things parked here:
//
//   read_full_file        — slurp an io_stream into a byte vector.
//   read_chunk_bytes      — copy an iff::chunk_event's payload into a
//                           freestanding byte vector.
//   find_form_child_offset— scan a fully-loaded IFF FORM for a specific
//                           top-level child chunk (used when libiff's
//                           handler API doesn't surface absolute offsets).
//   pcm_buffer            — shared ownership of a raw int8 PCM buffer.
//   pcm_audio_decoder     — minimal musac::decoder for raw signed 8-bit
//                           interleaved PCM, used by the AnimFX (amiga_anim)
//                           and CDXL audio paths.
//   bitplanar_to_chunky   — plane-major bitplane → chunky 8-bit converter
//                           (CDXL BIT_PLANAR / YAFA planar layout). Note
//                           amiga_anim's row-interleaved planar uses
//                           anim::planar_interleaved_to_chunky from
//                           lib_amiga_anim — different layout, stays
//                           separate.
//   ham_row_to_rgb888     — single-row HAM6/HAM8 renderer with a flag for
//                           the channel-expansion convention (replicate
//                           vs keep-prev's-low-bits — see notes below).

#include <musac/sdk/decoder.hh>
#include <musac/sdk/io_stream.hh>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include <iff/chunk_iterator.hh>

namespace onyx_anim::detail {
    // ----- Stream / chunk helpers ---------------------------------------------

    [[nodiscard]] inline bool
    read_full_file(musac::io_stream* s, std::vector <std::uint8_t>& out) {
        const auto sz = s->get_size();
        if (sz < 0) return false;
        out.resize(static_cast <std::size_t>(sz));
        if (s->seek(0, musac::seek_origin::set) < 0) return false;
        return s->read(out.data(), out.size()) == out.size();
    }

    // Works for both iff::chunk_event and iff::chunk_iterator::chunk_info,
    // which share `reader` field shape but have distinct types.
    template<typename ChunkEvent>
    [[nodiscard]] inline std::vector <std::uint8_t>
    read_chunk_bytes(ChunkEvent const& e) {
        if (!e.reader) return {};
        auto raw = e.reader->read_all();
        std::vector <std::uint8_t> out(raw.size());
        std::memcpy(out.data(), raw.data(), raw.size());
        return out;
    }

    // Locate a specific top-level child chunk inside a FORM-wrapped IFF buffer
    // (the buffer must start with "FORM <size> <form_type>" and the child has
    // 4-cc id `child_id`). Returns the absolute byte offset of the chunk's
    // payload (after its 8-byte header) and the size of that payload, or
    // {0, 0} if not found. Used when libiff's handler API doesn't expose
    // absolute offsets.
    struct chunk_range {
        std::size_t offset;
        std::size_t size;
    };

    [[nodiscard]] inline chunk_range
    find_form_child_offset(std::span <const std::uint8_t> file_bytes,
                           const char (& form_type)[5],
                           const char (& child_id)[5]) noexcept {
        if (file_bytes.size() < 12u) return {0, 0};
        if (std::memcmp(file_bytes.data(), "FORM", 4) != 0) return {0, 0};
        if (std::memcmp(file_bytes.data() + 8, form_type, 4) != 0) return {0, 0};
        auto rd_u32be = [](const std::uint8_t* p) noexcept {
            return (static_cast <std::uint32_t>(p[0]) << 24) |
                   (static_cast <std::uint32_t>(p[1]) << 16) |
                   (static_cast <std::uint32_t>(p[2]) << 8) |
                   static_cast <std::uint32_t>(p[3]);
        };
        const std::uint32_t outer_size = rd_u32be(file_bytes.data() + 4);
        const std::size_t outer_end =
            std::min <std::size_t>(file_bytes.size(), 8u + outer_size);

        std::size_t pos = 12u; // skip "FORM <size> <type>"
        while (pos + 8u <= outer_end) {
            const auto* p = file_bytes.data() + pos;
            const std::uint32_t sz = rd_u32be(p + 4);
            if (pos + 8u + sz > outer_end) break;
            if (std::memcmp(p, child_id, 4) == 0) {
                return {pos + 8u, sz};
            }
            pos += 8u + sz + (sz & 1u); // IFF padding
        }
        return {0, 0};
    }

    // ----- Pre-loaded raw PCM as a musac decoder ------------------------------
    //
    // Used by codecs that pre-build a single PCM buffer at open() time and want
    // to hand it to musac via take_audio_track(). The buffer is owned via
    // shared_ptr so it survives the codec being destroyed while an audio_source
    // the player constructed is still in flight.

    using pcm_buffer = std::shared_ptr <const std::vector <std::int8_t>>;

    class pcm_audio_decoder final : public musac::decoder {
        public:
            pcm_audio_decoder(const char* name,
                              musac::sample_rate_t rate,
                              musac::channels_t channels,
                              pcm_buffer owned_bytes) noexcept
                : name_(name),
                  rate_(rate),
                  channels_(channels),
                  owned_bytes_(std::move(owned_bytes)) {
            }

            [[nodiscard]] const char* get_name() const override { return name_; }

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
                    static_cast <std::int64_t>(owned_bytes_->size() / channels_);
                return std::chrono::microseconds{
                    sample_frames * 1'000'000LL /
                    static_cast <std::int64_t>(rate_)
                };
            }

            bool seek_to_time(std::chrono::microseconds pos) override {
                if (!stream_ || !rate_ || !channels_) return false;
                const std::int64_t sample_frames =
                    std::max <std::int64_t>(0, pos.count()) *
                    static_cast <std::int64_t>(rate_) / 1'000'000LL;
                // 1 byte per sample for 8-bit; channels samples per frame.
                const std::int64_t byte_offset =
                    sample_frames * static_cast <std::int64_t>(channels_);
                return stream_->seek(byte_offset, musac::seek_origin::set) >= 0;
            }

        protected:
            std::size_t do_decode(float* buf, std::size_t len,
                                  bool& call_again) override {
                if (!stream_ || !channels_) {
                    call_again = false;
                    return 0;
                }
                std::vector <std::int8_t> tmp(len);
                const auto got = stream_->read(tmp.data(), tmp.size());
                for (std::size_t i = 0; i < got; ++i) {
                    buf[i] = static_cast <float>(tmp[i]) / 128.0f;
                }
                call_again = got == tmp.size();
                return got;
            }

        private:
            const char* name_ = nullptr;
            musac::sample_rate_t rate_ = 0;
            musac::channels_t channels_ = 0;
            pcm_buffer owned_bytes_;
            musac::io_stream* stream_ = nullptr;
    };

    // ----- Plane-major bitplane → chunky --------------------------------------
    //
    // Source layout: all rows of plane 0 contiguously, then all rows of plane 1,
    // etc. (CDXL BIT_PLANAR, YAFA planar). Each plane row is
    // `bytes_per_plane_row` bytes (typically rounded up to a 16-bit word for
    // Amiga hardware compatibility). Pixels within a byte are MSB-first.
    //
    // NB: amiga_anim's row-interleaved planar (the ILBM convention) is a
    // different layout — handled by `anim::planar_interleaved_to_chunky` in
    // lib_amiga_anim/src/decoders.cc; do not unify the two.

    inline void bitplanar_to_chunky(const std::uint8_t* src,
                                    std::size_t bytes_per_plane_row,
                                    unsigned int planes,
                                    unsigned int width,
                                    unsigned int height,
                                    std::uint8_t* dst) noexcept {
        std::memset(dst, 0,
                    static_cast <std::size_t>(width) *
                    static_cast <std::size_t>(height));
        for (unsigned int p = 0; p < planes; ++p) {
            for (unsigned int y = 0; y < height; ++y) {
                const std::uint8_t* row = src +
                                          (static_cast <std::size_t>(p) * height +
                                           static_cast <std::size_t>(y)) * bytes_per_plane_row;
                std::uint8_t* out_row = dst +
                                        static_cast <std::size_t>(y) * width;
                for (unsigned int x = 0; x < width; ++x) {
                    const auto bit = static_cast <std::uint8_t>(
                        (row[x >> 3u] >> (7u - (x & 0x7u))) & 1u);
                    out_row[x] = static_cast <std::uint8_t>(out_row[x] | (bit << p));
                }
            }
        }
    }

    // ----- HAM6 / HAM8 channel-expansion --------------------------------------
    //
    // Render one chunky row of HAM6/HAM8 indices into 8-bit-per-channel RGB.
    // Top 2 bits of each pixel are a mode (0 = hold-from-palette,
    // 1 = modify-blue, 2 = modify-red, 3 = modify-green); the lower bits are
    // the value.
    //
    // The channel-expansion convention varies between formats:
    //   ham8_keep_prev_low_bits == false (amiga_anim ANIM HAM8, YAFA HAM8):
    //     replicate — 8-bit = (val << sl) | (val >> hold_bits)
    //   ham8_keep_prev_low_bits == true  (CDXL HAM8 per ffmpeg's libavcodec/cdxl.c):
    //     keep — 8-bit = (val << 2) | (prev & 3)
    // HAM6 is always replicate (val * 0x11), independent of the flag.
    //
    // Verified against ffmpeg's libavcodec/{iff,cdxl}.c via cross-check tests
    // in both directions.

    inline void ham_row_to_rgb888(const std::uint8_t* src_row,
                                  unsigned int width,
                                  unsigned int planes, // 6 or 8
                                  const std::uint8_t* palette_888,
                                  bool ham8_keep_prev_low_bits,
                                  std::uint8_t* dst_row) noexcept {
        const bool ham8 = (planes == 8);
        const unsigned int hold_bits = ham8 ? 6u : 4u;
        const auto mode_shift = static_cast <std::uint8_t>(hold_bits);
        const auto val_mask =
            static_cast <std::uint8_t>((1u << hold_bits) - 1u);
        const unsigned int sl = 8u - hold_bits;
        const auto prev_mask =
            static_cast <std::uint8_t>((1u << sl) - 1u); // 0x03 for HAM8

        std::uint8_t r = palette_888[0];
        std::uint8_t g = palette_888[1];
        std::uint8_t b = palette_888[2];
        for (unsigned int x = 0; x < width; ++x) {
            const std::uint8_t v = src_row[x];
            const auto mode = static_cast <std::uint8_t>(v >> mode_shift);
            const auto val = static_cast <std::uint8_t>(v & val_mask);
            if (mode == 0) {
                const std::size_t pi = static_cast <std::size_t>(val) * 3u;
                r = palette_888[pi + 0];
                g = palette_888[pi + 1];
                b = palette_888[pi + 2];
            } else {
                const auto shifted = static_cast <std::uint8_t>(val << sl);
                auto modify = [&](std::uint8_t prev) -> std::uint8_t {
                    if (ham8 && ham8_keep_prev_low_bits) {
                        return static_cast <std::uint8_t>(shifted | (prev & prev_mask));
                    }
                    // Replicate the value bits (HAM6 always; HAM8 ANIM/YAFA flavour).
                    return static_cast <std::uint8_t>(shifted | (shifted >> hold_bits));
                };
                if (mode == 1) b = modify(b);
                else if (mode == 2) r = modify(r);
                else g = modify(g);
            }
            dst_row[x * 3 + 0] = r;
            dst_row[x * 3 + 1] = g;
            dst_row[x * 3 + 2] = b;
        }
    }
} // namespace onyx_anim::detail
