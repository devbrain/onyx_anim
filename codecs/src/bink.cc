#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <bink/decoders.hh>
#include <bink/header.hh>

#include <musac/sdk/io_stream.hh>

#include "bink.hh"
#include "codec_common.hh"

namespace onyx_anim {
    namespace {
        constexpr std::array <std::string_view, 2> kBinkExtensions = {
            ".bik", ".bk"
        };

        using detail::read_full_file;

        // ITU-R BT.601 limited-range YUV → full-range RGB888 with the
        // standard 8-bit fixed-point matrix. NB this differs from
        // ffmpeg's libswscale output by ±1 count on a small fraction of
        // pixels (ffmpeg uses higher-precision lookup tables and floors
        // rather than rounding-half-up). For visual playback the
        // difference is imperceptible; for cross-checking we use the
        // YUV-roundtrip path in scripts/bink_cross_check.sh which
        // tolerates this conversion-induced noise.
        inline std::uint8_t clamp_byte(int x) noexcept {
            return static_cast <std::uint8_t>(std::max(0, std::min(255, x)));
        }

        void yuv420_to_rgb888(const std::uint8_t* y_plane, std::size_t y_stride,
                              const std::uint8_t* u_plane, std::size_t u_stride,
                              const std::uint8_t* v_plane, std::size_t v_stride,
                              unsigned int width, unsigned int height,
                              std::uint8_t* dst, std::size_t dst_stride) {
            for (unsigned int j = 0; j < height; ++j) {
                const std::uint8_t* y_row = y_plane + j * y_stride;
                const std::uint8_t* u_row = u_plane + (j >> 1u) * u_stride;
                const std::uint8_t* v_row = v_plane + (j >> 1u) * v_stride;
                std::uint8_t* d = dst + j * dst_stride;
                for (unsigned int i = 0; i < width; ++i) {
                    const int y = y_row[i] - 16;
                    const int u = u_row[i >> 1u] - 128;
                    const int v = v_row[i >> 1u] - 128;
                    const int r = (298 * y           + 409 * v + 128) >> 8;
                    const int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
                    const int b = (298 * y + 516 * u           + 128) >> 8;
                    d[i * 3 + 0] = clamp_byte(r);
                    d[i * 3 + 1] = clamp_byte(g);
                    d[i * 3 + 2] = clamp_byte(b);
                }
            }
        }

        // ----- Decoder --------------------------------------------------------

        class bink_decoder_impl final : public anim_decoder {
            public:
                using anim_decoder::open;

                [[nodiscard]] std::string_view name() const noexcept override {
                    return bink_decoder::codec_name;
                }
                [[nodiscard]] std::span <const std::string_view>
                extensions() const noexcept override {
                    return {kBinkExtensions.data(), kBinkExtensions.size()};
                }

                [[nodiscard]] bool sniff(musac::io_stream* s) const override {
                    if (!s) return false;
                    const auto pos = s->tell();
                    std::uint8_t magic[4] = {};
                    const auto n = s->read(magic, sizeof(magic));
                    s->seek(pos, musac::seek_origin::set);
                    if (n < sizeof(magic)) return false;
                    // Accept all Bink1 revisions; reject Bink2 (KB2*).
                    if (!(magic[0] == 'B' && magic[1] == 'I' && magic[2] == 'K')) {
                        return false;
                    }
                    const char rev = static_cast <char>(magic[3]);
                    return rev == 'b' || rev == 'f' || rev == 'g' || rev == 'h' ||
                           rev == 'i' || rev == 'k';
                }

                [[nodiscard]] result open(musac::io_stream* s,
                                          const decode_options& opts) override {
                    if (!s) return make_unexpected<error_type>("bink: null stream");
                    if (!read_full_file(s, file_bytes_)) {
                        return make_unexpected<error_type>("bink: cannot read file");
                    }

                    auto h = bink::parse_file_header(
                        {file_bytes_.data(), file_bytes_.size()});
                    if (!h) return make_unexpected<error_type>(h.error());
                    header_ = *h;

                    if (header_.version == 'b') {
                        // BIK[b] uses a different bundle layout / quant
                        // tables — implementation deferred; reject so the
                        // user gets a clear error rather than scrambled
                        // pixels.
                        return make_unexpected<error_type>(
                            "bink: BIK[b] revision not yet supported");
                    }
                    if (header_.has_alpha) {
                        return make_unexpected<error_type>(
                            "bink: alpha-plane files not yet supported");
                    }

                    if (header_.width  > opts.max_width ||
                        header_.height > opts.max_height) {
                        return make_unexpected<error_type>(
                            "bink: dimensions exceed configured limit");
                    }

                    bink::frame_state_init(state_, header_.width, header_.height);

                    // anim_info.
                    info_.width       = header_.width;
                    info_.height      = header_.height;
                    info_.frame_count = static_cast <unsigned int>(header_.frames.size());
                    info_.format      = pixel_format::rgb888;
                    const auto period_us =
                        bink::frame_period_us(header_.fps_num, header_.fps_den);
                    info_.frame_period = std::chrono::microseconds(period_us);
                    info_.duration = info_.frame_period * info_.frame_count;
                    info_.audio_track_count = 0u; // audio support deferred

                    cursor_ = 0;
                    return {};
                }

                [[nodiscard]] const anim_info& info() const noexcept override {
                    return info_;
                }

                [[nodiscard]] frame_result decode_frame(onyx_image::surface& out) override {
                    if (cursor_ >= header_.frames.size()) {
                        return make_unexpected<error_type>("bink: end of stream");
                    }
                    const auto& fe = header_.frames[cursor_];
                    if (fe.offset + fe.size > file_bytes_.size()) {
                        return make_unexpected<error_type>(
                            "bink: frame range past end of file");
                    }
                    auto frame_bytes = std::span <const std::uint8_t>(
                        file_bytes_.data() + fe.offset, fe.size);

                    // Strip per-track audio chunks. Each chunk:
                    //   u32 chunk_size  (size of the audio payload only,
                    //                    excluding this 4-byte header)
                    //   payload[chunk_size]
                    // Audio decode is deferred — we just skip past every
                    // chunk to reach the video bit stream.
                    for (std::size_t i = 0; i < header_.audio.size(); ++i) {
                        if (frame_bytes.size() < 4u) {
                            return make_unexpected<error_type>(
                                "bink: audio chunk size truncated");
                        }
                        const std::uint32_t chunk_sz =
                            static_cast <std::uint32_t>(frame_bytes[0]) |
                            (static_cast <std::uint32_t>(frame_bytes[1]) << 8u) |
                            (static_cast <std::uint32_t>(frame_bytes[2]) << 16u) |
                            (static_cast <std::uint32_t>(frame_bytes[3]) << 24u);
                        if (chunk_sz + 4u > frame_bytes.size()) {
                            return make_unexpected<error_type>(
                                "bink: audio chunk size out of range");
                        }
                        frame_bytes = frame_bytes.subspan(4u + chunk_sz);
                    }

                    if (auto r = bink::decode_frame(state_, frame_bytes, header_); !r) {
                        return make_unexpected<error_type>(r.error());
                    }

                    if (!out.set_size(static_cast <int>(header_.width),
                                      static_cast <int>(header_.height),
                                      pixel_format::rgb888)) {
                        return make_unexpected<error_type>("bink: surface set_size failed");
                    }

                    // Convert YUV420 → RGB888 row by row.
                    rgb_buffer_.assign(
                        static_cast <std::size_t>(header_.width) *
                        header_.height * 3u, 0);
                    const auto& cur = *state_.cur;
                    const std::size_t y_stride = cur.width;
                    // Chroma stride must match what `decode_plane` used —
                    // (((W+1)>>1)+7) & ~7. The earlier formula
                    // ((W+15)>>1+7) & ~7 was off by 8 for some widths.
                    const std::size_t c_stride =
                        (((header_.width + 1u) >> 1u) + 7u) & ~7u;
                    yuv420_to_rgb888(
                        cur.y.data(), y_stride,
                        cur.u.data(), c_stride,
                        cur.v.data(), c_stride,
                        header_.width, header_.height,
                        rgb_buffer_.data(),
                        static_cast <std::size_t>(header_.width) * 3u);
                    for (unsigned int row = 0; row < header_.height; ++row) {
                        // count is a BYTE count per surface.hpp — for
                        // RGB888 that's width × 3.
                        out.write_pixels(0, static_cast <int>(row),
                            static_cast <int>(header_.width) * 3,
                            rgb_buffer_.data() +
                                static_cast <std::size_t>(row) * header_.width * 3u);
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
                    return cursor_ >= header_.frames.size();
                }

                bool rewind() override { return seek_to_frame(0u); }

                bool seek_to_frame(unsigned int idx) override {
                    if (header_.frames.empty()) return false;
                    if (idx > header_.frames.size()) return false;
                    if (idx == cursor_) return true;
                    if (idx < cursor_) {
                        // Bink is delta-coded — we need a full replay from
                        // the previous keyframe. Find it.
                        unsigned int kf = idx;
                        while (kf > 0 && !header_.frames[kf].keyframe) --kf;
                        cursor_ = kf;
                        bink::frame_state_init(state_, header_.width, header_.height);
                    }
                    onyx_image::memory_surface tmp;
                    while (cursor_ < idx) {
                        if (!decode_frame(tmp)) return false;
                    }
                    return true;
                }

                bool seek_to_time(std::chrono::microseconds pts) override {
                    if (info_.frame_period.count() <= 0) return false;
                    if (header_.frames.empty()) return false;
                    std::int64_t f = pts.count() < 0
                        ? 0
                        : pts.count() / info_.frame_period.count();
                    const auto last =
                        static_cast <std::int64_t>(header_.frames.size());
                    if (f > last) f = last;
                    return seek_to_frame(static_cast <unsigned int>(f));
                }

            private:
                std::vector <std::uint8_t> file_bytes_;
                bink::file_header          header_{};
                bink::frame_state          state_{};
                std::vector <std::uint8_t> rgb_buffer_;

                anim_info                  info_{};
                std::size_t                cursor_ = 0;
        };
    } // namespace

    std::unique_ptr <anim_decoder> bink_decoder::create() {
        return std::make_unique <bink_decoder_impl>();
    }
} // namespace onyx_anim
