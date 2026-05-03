#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

#include <atari_seq/header.hh>
#include <atari_seq/decoders.hh>

#include "atari_seq.hh"

namespace onyx_anim {
    namespace {
        constexpr std::array<std::string_view, 1> kSeqExtensions = {".seq"};

        [[nodiscard]] bool read_exact(musac::io_stream* s, std::uint8_t* out, std::size_t n) {
            return s->read(out, n) == n;
        }

        class atari_seq_decoder_impl final : public anim_decoder {
            public:
                using anim_decoder::open;

                [[nodiscard]] std::string_view name() const noexcept override {
                    return atari_seq_decoder::codec_name;
                }

                [[nodiscard]] std::span<const std::string_view> extensions() const noexcept override {
                    return {kSeqExtensions.data(), kSeqExtensions.size()};
                }

                [[nodiscard]] bool sniff(musac::io_stream* s) const override {
                    if (!s) return false;
                    const auto pos = s->tell();
                    std::uint8_t buf[2] = {};
                    const auto n = s->read(buf, sizeof(buf));
                    s->seek(pos, musac::seek_origin::set);
                    if (n < 2) return false;
                    // Big-endian magic.
                    const std::uint16_t magic =
                        static_cast<std::uint16_t>((static_cast<unsigned>(buf[0]) << 8u) | buf[1]);
                    return magic == atari_seq::kMagicCyber ||
                           magic == atari_seq::kMagicFlicker;
                }

                [[nodiscard]] result open(musac::io_stream* s,
                                          const decode_options& opts) override {
                    if (!s) return make_unexpected<error_type>("seq: null stream");
                    stream_ = s;

                    // ---- 1. Read & parse the file header.
                    if (s->seek(0, musac::seek_origin::set) < 0) {
                        return make_unexpected<error_type>("seq: cannot seek to start");
                    }
                    std::array<std::uint8_t, atari_seq::kFileHeaderSize> hdr{};
                    if (!read_exact(s, hdr.data(), hdr.size())) {
                        return make_unexpected<error_type>("seq: file header truncated");
                    }
                    auto fh = atari_seq::parse_file_header(hdr);
                    if (!fh) return make_unexpected<error_type>(fh.error());
                    file_header_ = *fh;

                    if (file_header_.frame_count == 0) {
                        return make_unexpected<error_type>("seq: zero frame count");
                    }

                    // ---- 2. Skip the (sometimes corrupt) on-disk offsets table.
                    //
                    // Some real-world SEQ files (e.g., the COLAWARS demo set)
                    // have offsets that are off by a few bytes; Randelshofer's
                    // reference decoder never consults the table — it walks
                    // frames sequentially. We do the same below.
                    if (s->seek(
                            static_cast<std::int64_t>(file_header_.frame_count) * 4LL,
                            musac::seek_origin::cur) < 0) {
                        return make_unexpected<error_type>("seq: cannot skip offsets table");
                    }

                    // ---- 3. Walk every frame to build a reliable offsets index.
                    //
                    // For each frame we record the absolute file offset of its
                    // 128-byte cel header. The first frame sits where the
                    // stream is positioned now (right after the offsets table).
                    frame_offsets_.clear();
                    frame_offsets_.reserve(file_header_.frame_count);

                    std::array<std::uint8_t, atari_seq::kCelHeaderSize> cel_hdr{};
                    auto first_off = s->tell();
                    if (first_off < 0) {
                        return make_unexpected<error_type>("seq: tell failed");
                    }
                    if (!read_exact(s, cel_hdr.data(), cel_hdr.size())) {
                        return make_unexpected<error_type>("seq: first cel header truncated");
                    }
                    auto ch = atari_seq::parse_cel_header(cel_hdr);
                    if (!ch) return make_unexpected<error_type>(ch.error());
                    auto rinfo = atari_seq::info_for_resolution(ch->resolution);
                    if (!rinfo) return make_unexpected<error_type>(rinfo.error());

                    frame_offsets_.push_back(static_cast<std::uint32_t>(first_off));

                    // Skip frame-0 data, then walk the rest.
                    if (s->seek(static_cast<std::int64_t>(ch->data_size),
                                musac::seek_origin::cur) < 0) {
                        return make_unexpected<error_type>("seq: cannot skip frame 0 body");
                    }
                    for (std::uint32_t i = 1; i < file_header_.frame_count; ++i) {
                        const auto off = s->tell();
                        if (off < 0) return make_unexpected<error_type>("seq: tell failed");

                        if (!read_exact(s, cel_hdr.data(), cel_hdr.size())) {
                            return make_unexpected<error_type>("seq: cel header truncated mid-walk");
                        }
                        auto chN = atari_seq::parse_cel_header(cel_hdr);
                        if (!chN) return make_unexpected<error_type>(chN.error());
                        frame_offsets_.push_back(static_cast<std::uint32_t>(off));
                        if (s->seek(static_cast<std::int64_t>(chN->data_size),
                                    musac::seek_origin::cur) < 0) {
                            return make_unexpected<error_type>("seq: cannot skip frame body");
                        }
                    }

                    // ---- 4. Validate against decode_options limits.
                    if (rinfo->width  > opts.max_width ||
                        rinfo->height > opts.max_height) {
                        return make_unexpected<error_type>("seq: dimensions exceed configured limit");
                    }

                    // ---- 5. Populate anim_info.
                    info_.width        = rinfo->width;
                    info_.height       = rinfo->height;
                    info_.format       = pixel_format::indexed8;
                    info_.frame_count  = file_header_.frame_count;
                    // Speed → frame period: matches Animator Pro's reference
                    // SEQ reader (millisec_per_frame = 10 * speed / 60), i.e.
                    // 1 unit ≈ 166.67 µs. Earlier the unit was misread as
                    // 6 µs, which made playback ~28000× too fast.
                    info_.frame_period = std::chrono::microseconds(
                        static_cast<std::int64_t>(file_header_.speed) * 10000LL / 60LL);
                    info_.duration     = info_.frame_period * info_.frame_count;

                    // ---- 6. Allocate persistent planar framebuffer + chunky output.
                    res_           = *rinfo;
                    scanline_      = (res_.width + 7u) / 8u;
                    plane_stride_  = scanline_ * res_.height;
                    fb_planar_.assign(static_cast<std::size_t>(res_.planes) * plane_stride_, 0);
                    fb_chunky_.assign(static_cast<std::size_t>(res_.width) * res_.height, 0);
                    palette_.fill(0);

                    cursor_ = 0;
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
                    return cursor_ >= frame_offsets_.size();
                }

                bool rewind() override { return seek_to_frame(0); }

                bool seek_to_frame(unsigned int idx) override {
                    if (frame_offsets_.empty()) return false;
                    if (idx > frame_offsets_.size()) return false;
                    if (idx < cursor_) {
                        cursor_ = 0;
                        std::fill(fb_planar_.begin(), fb_planar_.end(), std::uint8_t{0});
                        std::fill(fb_chunky_.begin(), fb_chunky_.end(), std::uint8_t{0});
                        palette_.fill(0);
                    }
                    while (cursor_ < idx) {
                        if (!advance_state()) return false;
                    }
                    return true;
                }

                bool seek_to_time(std::chrono::microseconds pts) override {
                    if (info_.frame_period.count() <= 0) return false;
                    if (frame_offsets_.empty()) return false;
                    std::int64_t frame = (pts.count() < 0)
                        ? 0
                        : pts.count() / info_.frame_period.count();
                    if (frame < 0) frame = 0;
                    const auto last = static_cast<std::int64_t>(frame_offsets_.size());
                    if (frame > last) frame = last;
                    return seek_to_frame(static_cast<unsigned int>(frame));
                }

            private:
                [[nodiscard]] frame_result advance_state() {
                    if (cursor_ >= frame_offsets_.size()) {
                        return make_unexpected<error_type>("seq: end of stream");
                    }

                    if (stream_->seek(static_cast<std::int64_t>(frame_offsets_[cursor_]),
                                      musac::seek_origin::set) < 0) {
                        return make_unexpected<error_type>("seq: seek to frame failed");
                    }

                    std::array<std::uint8_t, atari_seq::kCelHeaderSize> cel_hdr{};
                    if (!read_exact(stream_, cel_hdr.data(), cel_hdr.size())) {
                        return make_unexpected<error_type>("seq: cel header truncated");
                    }
                    auto ch = atari_seq::parse_cel_header(cel_hdr);
                    if (!ch) return make_unexpected<error_type>(ch.error());

                    // Ingest the per-frame palette unconditionally — Randelshofer's
                    // decoder does the same.
                    atari_seq::decode_palette(ch->palette, res_.colors, palette_.data());

                    if (ch->data_size > 0) {
                        chunk_buf_.resize(ch->data_size);
                        if (!read_exact(stream_, chunk_buf_.data(), ch->data_size)) {
                            return make_unexpected<error_type>("seq: frame data truncated");
                        }
                        if (auto r = atari_seq::apply_frame(
                                std::span<const std::uint8_t>{chunk_buf_.data(), ch->data_size},
                                fb_planar_.data(),
                                scanline_,
                                plane_stride_,
                                res_.planes,
                                res_.width,
                                ch->x_offset, ch->y_offset, ch->width, ch->height,
                                ch->op, ch->sm); !r) {
                            return make_unexpected<error_type>(r.error());
                        }
                    }
                    // Empty frames (data_size == 0) leave the bitmap untouched,
                    // matching Randelshofer's reference behavior for both
                    // empty-copy and empty-xor cases.

                    // Convert planar → chunky into fb_chunky_.
                    atari_seq::planar_to_chunky(
                        fb_planar_.data(), scanline_, plane_stride_, res_.planes,
                        fb_chunky_.data(), res_.width,
                        res_.width, res_.height);

                    frame_info fi{};
                    fi.index    = static_cast<unsigned int>(cursor_);
                    fi.pts      = info_.frame_period * static_cast<std::int64_t>(cursor_);
                    fi.duration = info_.frame_period;
                    fi.palette_changed = true;       // SEQ frames always carry a palette
                    fi.keyframe        = (ch->op == atari_seq::operation::copy);
                    ++cursor_;
                    return fi;
                }

                [[nodiscard]] result present(onyx_image::surface& out) {
                    if (!out.set_size(static_cast<int>(res_.width),
                                      static_cast<int>(res_.height),
                                      pixel_format::indexed8)) {
                        return make_unexpected<error_type>("seq: surface set_size failed");
                    }
                    out.set_palette_size(static_cast<int>(res_.colors));
                    out.write_palette(0, std::span<const std::uint8_t>{
                        palette_.data(),
                        static_cast<std::size_t>(res_.colors) * 3u});
                    for (unsigned int y = 0; y < res_.height; ++y) {
                        out.write_pixels(
                            0,
                            static_cast<int>(y),
                            static_cast<int>(res_.width),
                            fb_chunky_.data() + static_cast<std::size_t>(y) * res_.width);
                    }
                    return {};
                }

                musac::io_stream*        stream_ = nullptr;
                atari_seq::file_header   file_header_{};
                atari_seq::resolution_info res_{};
                anim_info                info_{};
                std::vector<std::uint32_t> frame_offsets_;
                std::size_t              cursor_ = 0;

                std::size_t              scanline_ = 0;     // bytes per row in a plane
                std::size_t              plane_stride_ = 0; // bytes per plane
                std::vector<std::uint8_t> fb_planar_;
                std::vector<std::uint8_t> fb_chunky_;
                std::array<std::uint8_t, 768> palette_{};
                std::vector<std::uint8_t> chunk_buf_;
        };
    } // namespace

    std::unique_ptr<anim_decoder> atari_seq_decoder::create() {
        return std::make_unique<atari_seq_decoder_impl>();
    }
} // namespace onyx_anim
