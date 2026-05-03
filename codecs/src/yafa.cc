#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <yafa/header.hh>
#include <yafa/decoders.hh>

#include <iff/chunk_iterator.hh>
#include <iff/fourcc.hh>
#include <iff/handler_registry.hh>
#include <iff/parser.hh>

#include <musac/sdk/io_stream.hh>

#include "yafa.hh"
#include "codec_common.hh"

namespace onyx_anim {
    namespace {
        constexpr std::array <std::string_view, 1> kYafaExtensions = {".yafa"};

        using iff::operator""_4cc;
        constexpr auto kYafaCC = "YAFA"_4cc;
        constexpr auto kInfoCC = "INFO"_4cc;
        constexpr auto kDrgbCC = "DRGB"_4cc;
        constexpr auto kProfCC = "PROF"_4cc;
        constexpr auto kBodyCC = "BODY"_4cc;
        constexpr auto kTtblCC = "TTBL"_4cc;
        constexpr auto kAnnoCC = "ANNO"_4cc;

        // read_full_file, read_chunk_bytes, bitplanar_to_chunky and
        // ham_row_to_rgb888 live in codec_common.hh.
        using detail::read_full_file;
        using detail::read_chunk_bytes;

        class yafa_decoder_impl final : public anim_decoder {
            public:
                using anim_decoder::open;

                [[nodiscard]] std::string_view name() const noexcept override {
                    return yafa_decoder::codec_name;
                }

                [[nodiscard]] std::span <const std::string_view>
                extensions() const noexcept override {
                    return {kYafaExtensions.data(), kYafaExtensions.size()};
                }

                [[nodiscard]] bool sniff(musac::io_stream* s) const override {
                    if (!s) return false;
                    const auto pos = s->tell();
                    std::uint8_t buf[12] = {};
                    const auto n = s->read(buf, sizeof(buf));
                    s->seek(pos, musac::seek_origin::set);
                    if (n < 12) return false;
                    return buf[0] == 'F' && buf[1] == 'O' && buf[2] == 'R' && buf[3] == 'M' &&
                           buf[8] == 'Y' && buf[9] == 'A' && buf[10] == 'F' && buf[11] == 'A';
                }

                [[nodiscard]] result open(musac::io_stream* s,
                                          const decode_options& opts) override {
                    if (!s) return make_unexpected <error_type>("yafa: null stream");
                    if (!read_full_file(s, file_bytes_)) {
                        return make_unexpected <error_type>("yafa: cannot read file");
                    }

                    if (auto r = parse_iff_tree(); !r) {
                        return make_unexpected <error_type>(r.error());
                    }
                    if (!info_y_) {
                        return make_unexpected <error_type>("yafa: INFO chunk missing");
                    }
                    if (body_offset_ == 0u) {
                        return make_unexpected <error_type>("yafa: BODY chunk missing");
                    }

                    const auto& iy = *info_y_;
                    if (iy.width > opts.max_width ||
                        iy.height > opts.max_height) {
                        return make_unexpected <error_type>(
                            "yafa: dimensions exceed configured limit");
                    }
                    if (iy.frames == 0u) {
                        return make_unexpected <error_type>("yafa: zero frames");
                    }

                    // Build per-frame body offsets. PROF is mandatory for
                    // anything that varies in compressed size; for fixed-size
                    // frames we synthesise the offsets.
                    if (prof_offsets_.empty()) {
                        const auto fixed_size = uncompressed_frame_size();
                        if (fixed_size == 0u) {
                            return make_unexpected <error_type>(
                                "yafa: PROF required for compressed frames");
                        }
                        prof_offsets_.resize(iy.frames);
                        for (unsigned int i = 0; i < iy.frames; ++i) {
                            prof_offsets_[i] = static_cast <std::uint32_t>(
                                fixed_size * (i + 1));
                        }
                    } else if (prof_offsets_.size() != iy.frames) {
                        return make_unexpected <error_type>(
                            "yafa: PROF entry count != INFO.frames");
                    }

                    // Surface info.
                    info_.width = iy.width;
                    info_.height = iy.height;
                    info_.frame_count = iy.frames;
                    info_.format = iy.ham
                                       ? pixel_format::rgb888
                                       : pixel_format::indexed8;
                    // PAL: 50 video frames/sec; speed = video frames per anim frame.
                    const auto fps = (iy.speed > 0u)
                                         ? (50.0 / static_cast <double>(iy.speed))
                                         : 12.0;
                    info_.frame_period = std::chrono::microseconds(
                        static_cast <std::int64_t>(1'000'000.0 / fps));
                    info_.duration = info_.frame_period * info_.frame_count;

                    // Working buffers.
                    bytes_per_row_ = (static_cast <std::size_t>(iy.width) + 7u) / 8u;
                    bytes_per_plane_ = bytes_per_row_ * iy.height;
                    planar_size_ = bytes_per_plane_ * iy.depth;
                    chunky_size_ = static_cast <std::size_t>(iy.width) * iy.height;

                    // For delta-compressed planar, keep two buffers (frame
                    // N-2 / N-1) and ping-pong like ANIM op7.
                    fb_a_.assign(planar_size_, 0);
                    fb_b_.assign(planar_size_, 0);
                    chunky_buf_.assign(chunky_size_, 0);
                    if (iy.ham) {
                        rgb_buf_.assign(static_cast <std::size_t>(iy.width) *
                                        iy.height * 3u, 0);
                    }
                    palette_888_.assign(256u * 3u, 0);
                    if (!drgb_palette_.empty()) {
                        const auto n = std::min <std::size_t>(
                            drgb_palette_.size(), palette_888_.size());
                        std::memcpy(palette_888_.data(),
                                    drgb_palette_.data(), n);
                    }

                    cursor_ = 0;
                    return {};
                }

                [[nodiscard]] const anim_info& info() const noexcept override {
                    return info_;
                }

                [[nodiscard]] frame_result decode_frame(onyx_image::surface& out) override {
                    if (!info_y_ || cursor_ >= prof_offsets_.size()) {
                        return make_unexpected <error_type>("yafa: end of stream");
                    }
                    const auto& iy = *info_y_;
                    const auto idx = cursor_;
                    const std::size_t frame_off = (idx == 0)
                                                      ? std::size_t{0}
                                                      : prof_offsets_[idx - 1];
                    const std::size_t frame_end = prof_offsets_[idx];
                    if (frame_end < frame_off ||
                        body_offset_ + frame_end > file_bytes_.size()) {
                        return make_unexpected <error_type>(
                            "yafa: frame range past end of file");
                    }
                    const auto frame_packed = std::span <const std::uint8_t>(
                        file_bytes_.data() + body_offset_ + frame_off,
                        frame_end - frame_off);

                    // Decompress (if XPK) into a working buffer.
                    std::vector <std::uint8_t> raw;
                    const bool xpk = (iy.type == yafa::frame_type::planar_xpk ||
                                      iy.type == yafa::frame_type::chunky_xpk);
                    if (xpk) {
                        auto r = yafa::xpk_decompress(frame_packed);
                        if (!r) return make_unexpected <error_type>(r.error());
                        raw = std::move(*r);
                    } else {
                        raw.assign(frame_packed.begin(), frame_packed.end());
                    }

                    // Dynamic palette: trailing LoadRGB32 structure, fixed
                    // size driven by the bit depth (count = 1 << depth).
                    std::size_t pixel_bytes = raw.size();
                    if (iy.dyn_palette) {
                        const auto colors =
                            static_cast <std::uint16_t>(1u << iy.depth);
                        const std::size_t pal_bytes =
                            4u + static_cast <std::size_t>(colors) * 12u + 4u;
                        if (raw.size() < pal_bytes) {
                            return make_unexpected <error_type>(
                                "yafa: dyn-palette overrun");
                        }
                        pixel_bytes = raw.size() - pal_bytes;
                        const auto pal_span = std::span <const std::uint8_t>(
                            raw.data() + pixel_bytes, pal_bytes - 4u);
                        auto p = yafa::parse_drgb(pal_span);
                        if (!p) return make_unexpected <error_type>(p.error());
                        std::fill(palette_888_.begin(), palette_888_.end(),
                                  std::uint8_t{0});
                        const auto n = std::min <std::size_t>(
                            p->size(), palette_888_.size());
                        std::memcpy(palette_888_.data(), p->data(), n);
                    }

                    // Place pixel bytes into the working buffer the right
                    // way for each frame_type.
                    const bool chunky =
                        iy.type == yafa::frame_type::chunky8 ||
                        iy.type == yafa::frame_type::chunky_xpk;

                    if (iy.delta) {
                        // Spec says frames 0 and 1 are "uncompressed", but
                        // every real-world YAFA writer we've seen emits
                        // deltas-from-zero for those too: frame 0's data
                        // begins with the same 8-plane-offsets table as any
                        // other delta frame. So we always run the delta
                        // path; the buffers start zero-initialised, which
                        // correctly seeds the SAME/UNIQ ops in frame 0.
                        //
                        // Double-buffered playback (fb_a_/fb_b_) follows the
                        // ANIM-op-7 / op-8 convention: delta in frame N is
                        // applied on top of fb_b_ (which holds frame N-2),
                        // produces frame N; we then swap so fb_b_ holds
                        // frame N-1 for next time.
                        const auto delta_span = std::span <const std::uint8_t>(
                            raw.data(), pixel_bytes);
                        if (auto r = yafa::yafa_apply_delta(
                            delta_span, fb_b_.data(),
                            iy.width, iy.height, iy.depth,
                            iy.delta_w); !r) {
                            return make_unexpected <error_type>(r.error());
                        }
                        std::memcpy(fb_a_.data(), fb_b_.data(), planar_size_);
                    } else if (chunky) {
                        if (pixel_bytes < chunky_size_) {
                            return make_unexpected <error_type>(
                                "yafa: chunky frame too small");
                        }
                        std::memcpy(chunky_buf_.data(), raw.data(), chunky_size_);
                    } else {
                        // Plain planar frame, no delta.
                        if (pixel_bytes < planar_size_) {
                            return make_unexpected <error_type>(
                                "yafa: planar frame too small");
                        }
                        std::memcpy(fb_a_.data(), raw.data(), planar_size_);
                    }

                    // Ensure chunky buffer reflects the current frame.
                    if (!chunky) {
                        detail::bitplanar_to_chunky(
                            fb_a_.data(), bytes_per_row_,
                            iy.depth, iy.width, iy.height,
                            chunky_buf_.data());
                    }

                    // Present.
                    if (iy.ham) {
                        if (!out.set_size(static_cast <int>(iy.width),
                                          static_cast <int>(iy.height),
                                          pixel_format::rgb888)) {
                            return make_unexpected <error_type>(
                                "yafa: surface set_size failed");
                        }
                        for (unsigned int y = 0; y < iy.height; ++y) {
                            const std::uint8_t* src = chunky_buf_.data() +
                                                      static_cast <std::size_t>(y) * iy.width;
                            std::uint8_t* row = rgb_buf_.data() +
                                                static_cast <std::size_t>(y) * iy.width * 3u;
                            detail::ham_row_to_rgb888(
                                src, iy.width, iy.depth,
                                palette_888_.data(),
                                /*ham8_keep_prev_low_bits=*/false,
                                row);
                            out.write_pixels(0, static_cast <int>(y),
                                             static_cast <int>(iy.width * 3u),
                                             row);
                        }
                    } else {
                        if (!out.set_size(static_cast <int>(iy.width),
                                          static_cast <int>(iy.height),
                                          pixel_format::indexed8)) {
                            return make_unexpected <error_type>(
                                "yafa: surface set_size failed");
                        }
                        out.set_palette_size(256);
                        out.write_palette(0, std::span <const std::uint8_t>{
                                              palette_888_.data(), palette_888_.size()
                                          });
                        for (unsigned int y = 0; y < iy.height; ++y) {
                            out.write_pixels(0, static_cast <int>(y),
                                             static_cast <int>(iy.width),
                                             chunky_buf_.data() +
                                             static_cast <std::size_t>(y) * iy.width);
                        }
                    }

                    // Swap the planar buffers if double-buffering for delta;
                    // for non-delta there's no historical state to keep.
                    if (iy.delta) {
                        std::swap(fb_a_, fb_b_);
                    }

                    frame_info fi{};
                    fi.index = static_cast <unsigned int>(idx);
                    fi.pts = info_.frame_period * static_cast <std::int64_t>(idx);
                    fi.duration = info_.frame_period;
                    fi.keyframe = !iy.delta || (idx < 2u);
                    ++cursor_;
                    return fi;
                }

                [[nodiscard]] bool eof() const noexcept override {
                    return !info_y_ || cursor_ >= prof_offsets_.size();
                }

                bool rewind() override { return seek_to_frame(0u); }

                bool seek_to_frame(unsigned int idx) override {
                    if (!info_y_ || idx > prof_offsets_.size()) return false;
                    if (idx == cursor_) return true;
                    // Delta-compressed playback can't jump arbitrarily
                    // (each frame depends on the prior pair). Re-seed
                    // by re-decoding from the start.
                    if (info_y_->delta && idx != 0u) {
                        cursor_ = 0;
                        std::fill(fb_a_.begin(), fb_a_.end(), std::uint8_t{0});
                        std::fill(fb_b_.begin(), fb_b_.end(), std::uint8_t{0});
                        std::fill(chunky_buf_.begin(), chunky_buf_.end(),
                                  std::uint8_t{0});
                        // Walk forward to land on `idx` next decode.
                        // We need a throwaway surface to fill — use a
                        // local one to avoid disturbing the caller's.
                        onyx_image::memory_surface tmp;
                        while (cursor_ < idx) {
                            auto r = decode_frame(tmp);
                            if (!r) return false;
                        }
                        return true;
                    }
                    cursor_ = idx;
                    return true;
                }

                bool seek_to_time(std::chrono::microseconds pts) override {
                    if (info_.frame_period.count() <= 0) return false;
                    if (!info_y_) return false;
                    std::int64_t f = pts.count() < 0
                                         ? 0
                                         : pts.count() / info_.frame_period.count();
                    const auto last =
                        static_cast <std::int64_t>(prof_offsets_.size());
                    if (f > last) f = last;
                    return seek_to_frame(static_cast <unsigned int>(f));
                }

            private:
                [[nodiscard]] result parse_iff_tree() {
                    std::string s(reinterpret_cast <const char*>(file_bytes_.data()),
                                  file_bytes_.size());
                    std::istringstream stream(s, std::ios::binary);

                    std::optional <error_type> first_error;
                    auto remember_error = [&](error_type msg) {
                        if (!first_error) first_error = std::move(msg);
                    };

                    iff::handler_registry handlers;
                    handlers.on_chunk_in_form(kYafaCC, kInfoCC,
                                              [&](const iff::chunk_event& e) {
                                                  if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                                                  auto bytes = read_chunk_bytes(e);
                                                  auto v = yafa::parse_info({bytes.data(), bytes.size()});
                                                  if (!v) {
                                                      remember_error(v.error());
                                                      return;
                                                  }
                                                  info_y_ = *v;
                                              });
                    handlers.on_chunk_in_form(kYafaCC, kDrgbCC,
                                              [&](const iff::chunk_event& e) {
                                                  if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                                                  auto bytes = read_chunk_bytes(e);
                                                  auto v = yafa::parse_drgb({bytes.data(), bytes.size()});
                                                  if (!v) {
                                                      remember_error(v.error());
                                                      return;
                                                  }
                                                  drgb_palette_ = std::move(*v);
                                              });
                    handlers.on_chunk_in_form(kYafaCC, kProfCC,
                                              [&](const iff::chunk_event& e) {
                                                  if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                                                  auto bytes = read_chunk_bytes(e);
                                                  auto v = yafa::parse_prof({bytes.data(), bytes.size()});
                                                  if (!v) {
                                                      remember_error(v.error());
                                                      return;
                                                  }
                                                  prof_offsets_ = std::move(*v);
                                              });
                    handlers.on_chunk_in_form(kYafaCC, kBodyCC,
                                              [&](const iff::chunk_event& e) {
                                                  if (e.type != iff::chunk_event_type::begin || !e.reader) return;
                                                  // We need the on-disk position of the BODY
                                                  // payload, not its bytes. The reader streams
                                                  // through the chunk so we capture the offset
                                                  // by way of `header.byte_offset` which the IFF
                                                  // library exposes via the chunk_header.
                                                  // (Alternative: scan file_bytes_ for the BODY
                                                  // chunk header — simpler since libiff doesn't
                                                  // surface absolute offsets.)
                                                  //
                                                  // The reader has consumed nothing yet at this
                                                  // point, so we know the chunk body sits in
                                                  // file_bytes_ — locate it by linear search.
                                                  (void)e; // value unused; locate below in finalize.
                                              });

                    try {
                        iff::parse(stream, handlers);
                    } catch (const std::exception& ex) {
                        return make_unexpected <error_type>(
                            std::string("yafa: IFF parse failed: ") + ex.what());
                    }
                    if (first_error) {
                        return make_unexpected <error_type>(*first_error);
                    }
                    // libiff's handler API doesn't expose absolute byte
                    // offsets; locate BODY's payload via the shared
                    // find_form_child_offset helper.
                    const auto body = detail::find_form_child_offset(
                        {file_bytes_.data(), file_bytes_.size()},
                        "YAFA", "BODY");
                    body_offset_ = body.offset;
                    return {};
                }

                [[nodiscard]] std::size_t uncompressed_frame_size() const noexcept {
                    if (!info_y_) return 0;
                    const auto& iy = *info_y_;
                    if (iy.type == yafa::frame_type::chunky_xpk ||
                        iy.type == yafa::frame_type::planar_xpk ||
                        iy.delta) {
                        return 0; // variable size — needs PROF
                    }
                    if (iy.type == yafa::frame_type::chunky8) {
                        return static_cast <std::size_t>(iy.width) * iy.height;
                    }
                    // planar
                    const std::size_t bpr = (static_cast <std::size_t>(iy.width) + 7u) / 8u;
                    return bpr * iy.height * iy.depth;
                }

                std::vector <std::uint8_t> file_bytes_;
                std::optional <yafa::info> info_y_;
                std::vector <std::uint8_t> drgb_palette_; // unscaled RGB triplets
                std::vector <std::uint32_t> prof_offsets_; // cumulative byte offsets
                std::size_t body_offset_ = 0;

                anim_info info_{};
                std::size_t cursor_ = 0;
                std::size_t bytes_per_row_ = 0;
                std::size_t bytes_per_plane_ = 0;
                std::size_t planar_size_ = 0;
                std::size_t chunky_size_ = 0;

                std::vector <std::uint8_t> fb_a_; // current planar (delta in)
                std::vector <std::uint8_t> fb_b_; // previous planar (delta from)
                std::vector <std::uint8_t> chunky_buf_;
                std::vector <std::uint8_t> rgb_buf_;
                std::vector <std::uint8_t> palette_888_;
        };
    } // namespace

    std::unique_ptr <anim_decoder> yafa_decoder::create() {
        return std::make_unique <yafa_decoder_impl>();
    }
} // namespace onyx_anim
