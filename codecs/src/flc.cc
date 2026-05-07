#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <bytes/bytes.hh>
#include <flc/header.hh>
#include <flc/chunks.hh>
#include <flc/decoders.hh>

#include "flc.hh"

namespace onyx_anim {
    namespace {
        constexpr std::array <std::string_view, 2> kFlcExtensions = {".flc", ".fli"};

        // Read exactly `n` bytes from the stream into `out`, returning false on
        // truncation or I/O error.
        [[nodiscard]] bool read_exact(musac::io_stream* s, std::uint8_t* out, std::size_t n) {
            return s->read(out, n) == n;
        }

        enum class compiled_op : std::uint8_t {
            literal,
            fill_byte,
            fill_word,
            store_byte,
        };

        struct compiled_command {
            compiled_op op = compiled_op::literal;
            std::uint32_t dst = 0;
            std::uint32_t count = 0;
            std::uint32_t src = 0;
            std::uint8_t lo = 0;
            std::uint8_t hi = 0;
        };

        struct compiled_sub_chunk {
            unsigned int sub_chunk_index = 0;
            std::vector <compiled_command> commands;
            std::vector <std::uint8_t> literals;
        };

        class flc_decoder_impl final : public anim_decoder {
            public:
                using anim_decoder::open; // expose 1-arg overload

                [[nodiscard]] std::string_view name() const noexcept override {
                    return flc_decoder::codec_name;
                }

                [[nodiscard]] std::span <const std::string_view> extensions() const noexcept override {
                    return {kFlcExtensions.data(), kFlcExtensions.size()};
                }

                [[nodiscard]] bool sniff(musac::io_stream* s) const override {
                    if (!s) {
                        return false;
                    }
                    const auto pos = s->tell();
                    std::uint8_t buf[6] = {};
                    const auto n = s->read(buf, sizeof(buf));
                    s->seek(pos, musac::seek_origin::set);
                    if (n < 6) {
                        return false;
                    }
                    const std::uint16_t magic =
                        static_cast <std::uint16_t>(buf[4]) |
                        static_cast <std::uint16_t>(buf[5] << 8);
                    return magic == flc::kMagicFlc || magic == flc::kMagicFli;
                }

                [[nodiscard]] result open(musac::io_stream* s,
                                          const decode_options& opts) override {
                    if (!s) {
                        return make_unexpected <error_type>("flc: null stream");
                    }
                    stream_ = s;

                    // ---- 1. Read the file header into an in-memory buffer.
                    if (s->seek(0, musac::seek_origin::set) < 0) {
                        return make_unexpected <error_type>("flc: cannot seek to start");
                    }
                    std::array <std::uint8_t, flc::kFileHeaderSize> hdr_bytes{};
                    const auto n_read = s->read(hdr_bytes.data(), hdr_bytes.size());
                    if (n_read < flc::kMinHeaderBytes) {
                        return make_unexpected <error_type>("flc: file too short for header");
                    }

                    // ---- 2. Parse it via lib_flc and propagate any error string.
                    auto parsed = flc::parse_file_header(
                        std::span <const std::uint8_t>{hdr_bytes.data(), n_read});
                    if (!parsed) {
                        return make_unexpected <error_type>(parsed.error());
                    }
                    file_header_ = *parsed;

                    // ---- 3. Validate dimensions against caller-provided limits.
                    if (file_header_.width > opts.max_width ||
                        file_header_.height > opts.max_height) {
                        return make_unexpected <error_type>("flc: dimensions exceed configured limit");
                    }

                    // ---- 4. Populate the public anim_info.
                    info_.width = file_header_.width;
                    info_.height = file_header_.height;
                    info_.format = pixel_format::indexed8;
                    info_.frame_count = file_header_.frame_count;
                    info_.frame_period = file_header_.is_fli()
                                             ? std::chrono::microseconds(
                                                 (file_header_.speed_units * 1'000'000ULL) / 70ULL)
                                             : std::chrono::microseconds(
                                                 file_header_.speed_units * 1000ULL);
                    info_.duration = info_.frame_period * info_.frame_count;

                    // ---- 5. Allocate the persistent framebuffer + palette.
                    fb_pitch_ = file_header_.width;
                    fb_.assign(static_cast <std::size_t>(fb_pitch_) *
                               static_cast <std::size_t>(file_header_.height),
                               0);
                    palette_.fill(0);

                    // ---- 6. Optionally walk frame headers to build a seek index.
                    if (opts.build_frame_index) {
                        if (auto r = build_frame_index(); !r) {
                            return r;
                        }
                    }

                    cursor_ = 0;
                    return {};
                }

                [[nodiscard]] const anim_info& info() const noexcept override { return info_; }

                [[nodiscard]] frame_result decode_frame(onyx_image::surface& out) override {
                    auto fi = advance_state();
                    if (!fi) return fi;
                    if (auto r = present(out, fi->palette_changed); !r) {
                        return make_unexpected <error_type>(r.error());
                    }
                    return fi;
                }

                [[nodiscard]] bool eof() const noexcept override {
                    return cursor_ >= frames_.size();
                }

                bool rewind() override {
                    return seek_to_frame(0);
                }

                bool seek_to_frame(unsigned int idx) override {
                    if (frames_.empty()) return false;
                    if (idx > frames_.size()) return false;

                    // Backward seek: clear persistent state and replay from frame 0.
                    // (We don't track which frames are keyframes; the conservative
                    // choice is to always start from the beginning.)
                    if (idx < cursor_) {
                        cursor_ = 0;
                        std::fill(fb_.begin(), fb_.end(), std::uint8_t{0});
                        palette_.fill(0);
                    }
                    while (cursor_ < idx) {
                        if (!advance_state()) {
                            return false;
                        }
                    }
                    return true;
                }

                bool seek_to_time(std::chrono::microseconds pts) override {
                    if (info_.frame_period.count() <= 0) return false;
                    if (frames_.empty()) return false;

                    // Snap negative PTS to frame 0; clamp past-end PTS to last frame.
                    std::int64_t frame = (pts.count() < 0)
                        ? 0
                        : pts.count() / info_.frame_period.count();
                    if (frame < 0) frame = 0;
                    const auto last = static_cast<std::int64_t>(frames_.size());
                    if (frame > last) frame = last;
                    return seek_to_frame(static_cast<unsigned int>(frame));
                }

            private:
                struct frame_entry {
                    std::int64_t  offset;
                    std::uint32_t size;
                    std::vector <compiled_sub_chunk> compiled;
                };

                /**
                 * Decode the frame at `cursor_` into the persistent fb_/palette_
                 * state, advance cursor_, and return its frame_info. Does NOT
                 * emit to a surface — that's `present()`'s job.
                 *
                 * Used by both decode_frame() (which then calls present()) and
                 * by seek_to_frame() (which discards the rendered pixels).
                 */
                [[nodiscard]] frame_result advance_state() {
                    if (cursor_ >= frames_.size()) {
                        return make_unexpected <error_type>("flc: end of stream");
                    }
                    const auto& fe = frames_[cursor_];

                    if (stream_->seek(fe.offset, musac::seek_origin::set) < 0) {
                        return make_unexpected <error_type>("flc: seek to frame failed");
                    }
                    chunk_buf_.resize(fe.size);
                    if (!read_exact(stream_, chunk_buf_.data(), fe.size)) {
                        return make_unexpected <error_type>("flc: frame chunk truncated");
                    }

                    auto fh = flc::parse_frame_header(
                        std::span <const std::uint8_t>{chunk_buf_.data(), flc::kFrameHeaderSize});
                    if (!fh) {
                        return make_unexpected <error_type>(fh.error());
                    }

                    bool palette_changed = false;
                    std::size_t compiled_index = 0;
                    std::size_t off = flc::kFrameHeaderSize;
                    for (unsigned int sc = 0; sc < fh->sub_chunks; ++sc) {
                        if (off + flc::kSubChunkHeaderSize > chunk_buf_.size()) {
                            return make_unexpected <error_type>("flc: sub-chunk header overflow");
                        }
                        auto sh = flc::parse_sub_chunk_header(
                            std::span <const std::uint8_t>{
                                chunk_buf_.data() + off, chunk_buf_.size() - off});
                        if (!sh) {
                            return make_unexpected <error_type>(sh.error());
                        }
                        if (off + sh->size > chunk_buf_.size()) {
                            return make_unexpected <error_type>("flc: sub-chunk payload overflow");
                        }
                        std::size_t payload_size = sh->size - flc::kSubChunkHeaderSize;
                        if (sh->type == flc::sub_chunk_type::copy &&
                            sc + 1u == fh->sub_chunks) {
                            const auto expected_copy_bytes =
                                static_cast <std::size_t>(file_header_.width) *
                                static_cast <std::size_t>(file_header_.height);
                            const auto available_in_frame =
                                chunk_buf_.size() - (off + flc::kSubChunkHeaderSize);
                            if (payload_size < expected_copy_bytes &&
                                available_in_frame >= expected_copy_bytes) {
                                payload_size = expected_copy_bytes;
                            }
                        }
                        const std::span <const std::uint8_t> payload{
                            chunk_buf_.data() + off + flc::kSubChunkHeaderSize,
                            payload_size};

                        if (sh->type == flc::sub_chunk_type::ss2 &&
                            compiled_index < fe.compiled.size() &&
                            fe.compiled[compiled_index].sub_chunk_index == sc) {
                            execute_compiled(fe.compiled[compiled_index]);
                            ++compiled_index;
                        } else if (auto r = dispatch_sub_chunk(sh->type, payload, palette_changed); !r) {
                            return make_unexpected <error_type>(r.error());
                        }
                        off += sh->size;
                    }

                    frame_info fi{};
                    fi.index    = static_cast <unsigned int>(cursor_);
                    fi.pts      = info_.frame_period * static_cast <std::int64_t>(cursor_);
                    fi.duration = (fh->delay > 0)
                        ? std::chrono::microseconds(static_cast <std::int64_t>(fh->delay) * 1000LL)
                        : info_.frame_period;
                    fi.palette_changed = palette_changed;
                    fi.keyframe        = (cursor_ == 0); // refined when we track chunk types

                    ++cursor_;
                    return fi;
                }

                [[nodiscard]] result dispatch_sub_chunk(flc::sub_chunk_type type,
                                                       std::span <const std::uint8_t> payload,
                                                       bool& palette_changed) {
                    switch (type) {
                        case flc::sub_chunk_type::brun:
                            return adapt(flc::decode_brun(payload, fb_.data(), fb_pitch_,
                                                          file_header_.width, file_header_.height));
                        case flc::sub_chunk_type::lc:
                            return adapt(flc::decode_lc(payload, fb_.data(), fb_pitch_,
                                                        file_header_.width, file_header_.height));
                        case flc::sub_chunk_type::ss2:
                            return adapt(flc::decode_ss2(payload, fb_.data(), fb_pitch_,
                                                         file_header_.width, file_header_.height));
                        case flc::sub_chunk_type::copy:
                            return adapt(flc::decode_copy(payload, fb_.data(), fb_pitch_,
                                                          file_header_.width, file_header_.height));
                        case flc::sub_chunk_type::black:
                            flc::decode_black(fb_.data(), fb_pitch_,
                                              file_header_.width, file_header_.height);
                            return {};
                        case flc::sub_chunk_type::color_256:
                            palette_changed = true;
                            return adapt(flc::decode_color_256(payload, palette_.data()));
                        case flc::sub_chunk_type::color_64:
                            palette_changed = true;
                            return adapt(flc::decode_color_64(payload, palette_.data()));
                        case flc::sub_chunk_type::pstamp:
                            // Skip postage-stamp preview chunks silently.
                            return {};
                    }
                    // Unknown sub-chunk type — skip silently for forward compat.
                    return {};
                }

                // Translate flc::result (also expected<void, std::string>) to onyx_anim's.
                [[nodiscard]] static result adapt(const flc::result& r) {
                    if (r) return {};
                    return make_unexpected <error_type>(r.error());
                }

                void execute_compiled(const compiled_sub_chunk& compiled) {
                    for (const auto& command : compiled.commands) {
                        std::uint8_t* dst = fb_.data() + command.dst;
                        switch (command.op) {
                            case compiled_op::literal:
                                std::memcpy(dst,
                                            compiled.literals.data() + command.src,
                                            command.count);
                                break;
                            case compiled_op::fill_byte:
                                std::memset(dst, command.lo, command.count);
                                break;
                            case compiled_op::fill_word:
                                for (std::uint32_t i = 0; i < command.count; ++i) {
                                    dst[i * 2u] = command.lo;
                                    dst[i * 2u + 1u] = command.hi;
                                }
                                break;
                            case compiled_op::store_byte:
                                *dst = command.lo;
                                break;
                        }
                    }
                }

                [[nodiscard]] bool compile_ss2_payload(std::span <const std::uint8_t> payload,
                                                        unsigned int sub_chunk_index,
                                                        compiled_sub_chunk& out) const {
                    const std::uint8_t* p = payload.data();
                    const std::uint8_t* const end = payload.data() + payload.size();
                    const auto has = [&p, end](std::size_t n) noexcept {
                        return static_cast <std::size_t>(end - p) >= n;
                    };

                    if (!has(2)) return false;
                    const auto line_count = static_cast <unsigned int>(bytes::read_u16le(p));
                    p += 2;

                    out.sub_chunk_index = sub_chunk_index;
                    unsigned int y = 0;
                    for (unsigned int li = 0; li < line_count; ++li) {
                        if (!has(2)) return false;
                        std::uint16_t opcode = bytes::read_u16le(p);
                        p += 2;
                        std::uint16_t packet_count = opcode;

                        if ((opcode & 0xC000u) != 0) {
                            for (;;) {
                                const unsigned tag = (opcode >> 14) & 0x3u;
                                if (tag == 0u) {
                                    packet_count = opcode;
                                    break;
                                }

                                if (tag == 0b11u) {
                                    const auto signed_op = static_cast <std::int16_t>(opcode);
                                    y += static_cast <unsigned int>(-signed_op);
                                    if (y > file_header_.height) return false;
                                } else if (tag == 0b10u) {
                                    if (y >= file_header_.height || file_header_.width == 0) {
                                        return false;
                                    }
                                    out.commands.push_back({compiled_op::store_byte,
                                                            static_cast <std::uint32_t>(
                                                                static_cast <std::size_t>(y) *
                                                                fb_pitch_ +
                                                                (file_header_.width - 1u)),
                                                            1,
                                                            0,
                                                            static_cast <std::uint8_t>(opcode & 0xFFu),
                                                            0});
                                } else {
                                    return false;
                                }

                                if (!has(2)) return false;
                                opcode = bytes::read_u16le(p);
                                p += 2;
                            }
                        }

                        if (y >= file_header_.height) return false;
                        unsigned int x = 0;
                        for (unsigned int packet = 0; packet < packet_count; ++packet) {
                            if (!has(2)) return false;
                            const auto col_skip = static_cast <unsigned int>(p[0]);
                            const auto rle_count = static_cast <std::int8_t>(p[1]);
                            p += 2;

                            x += col_skip;
                            if (x > file_header_.width) return false;
                            if (rle_count == 0) continue;

                            const auto dst = static_cast <std::uint32_t>(
                                static_cast <std::size_t>(y) * fb_pitch_ + x);
                            if (rle_count > 0) {
                                const auto n_words = static_cast <unsigned int>(rle_count);
                                const auto n_bytes = n_words * 2u;
                                if (!has(n_bytes)) return false;
                                if (n_bytes > file_header_.width - x) return false;

                                const auto src = static_cast <std::uint32_t>(out.literals.size());
                                out.literals.insert(out.literals.end(), p, p + n_bytes);
                                out.commands.push_back({compiled_op::literal,
                                                        dst,
                                                        static_cast <std::uint32_t>(n_bytes),
                                                        src,
                                                        0,
                                                        0});
                                p += n_bytes;
                                x += n_bytes;
                            } else {
                                const auto n_words = static_cast <unsigned int>(
                                    -static_cast <int>(rle_count));
                                const auto n_bytes = n_words * 2u;
                                if (!has(2)) return false;
                                if (n_bytes > file_header_.width - x) return false;

                                const std::uint8_t lo = p[0];
                                const std::uint8_t hi = p[1];
                                p += 2;
                                out.commands.push_back({lo == hi ? compiled_op::fill_byte
                                                                  : compiled_op::fill_word,
                                                        dst,
                                                        lo == hi
                                                            ? static_cast <std::uint32_t>(n_bytes)
                                                            : static_cast <std::uint32_t>(n_words),
                                                        0,
                                                        lo,
                                                        hi});
                                x += n_bytes;
                            }
                        }
                        ++y;
                    }
                    return true;
                }

                [[nodiscard]] std::vector <compiled_sub_chunk>
                    compile_frame_chunks(std::span <const std::uint8_t> frame_bytes) const {
                    std::vector <compiled_sub_chunk> out;
                    if (frame_bytes.size() < flc::kFrameHeaderSize) return out;

                    auto fh = flc::parse_frame_header(frame_bytes);
                    if (!fh) return out;

                    std::size_t off = flc::kFrameHeaderSize;
                    for (unsigned int sc = 0; sc < fh->sub_chunks; ++sc) {
                        if (off + flc::kSubChunkHeaderSize > frame_bytes.size()) break;

                        auto sh = flc::parse_sub_chunk_header(
                            std::span <const std::uint8_t>{
                                frame_bytes.data() + off, frame_bytes.size() - off});
                        if (!sh) break;
                        if (sh->size < flc::kSubChunkHeaderSize ||
                            off + sh->size > frame_bytes.size()) {
                            break;
                        }

                        const std::span <const std::uint8_t> payload{
                            frame_bytes.data() + off + flc::kSubChunkHeaderSize,
                            sh->size - flc::kSubChunkHeaderSize};
                        compiled_sub_chunk compiled;
                        bool ok = false;
                        if (sh->type == flc::sub_chunk_type::ss2) {
                            ok = compile_ss2_payload(payload, sc, compiled);
                        }
                        if (ok && !compiled.commands.empty()) {
                            out.push_back(std::move(compiled));
                        }
                        off += sh->size;
                    }

                    return out;
                }

                [[nodiscard]] result present(onyx_image::surface& out,
                                             bool palette_changed) {
                    if (auto* mem = dynamic_cast <onyx_image::memory_surface*>(&out)) {
                        const auto w = static_cast <int>(file_header_.width);
                        const auto h = static_cast <int>(file_header_.height);
                        if (mem->width() != w || mem->height() != h ||
                            mem->format() != pixel_format::indexed8) {
                            if (!mem->set_size(w, h, pixel_format::indexed8)) {
                                return make_unexpected <error_type>("flc: surface set_size failed");
                            }
                            palette_changed = true;
                        }

                        if (palette_changed || mem->palette().size() != palette_.size()) {
                            mem->set_palette_size(256);
                            mem->write_palette(0, std::span <const std::uint8_t>{
                                palette_.data(), palette_.size()});
                        }

                        auto pixels = mem->mutable_pixels();
                        if (pixels.size() < fb_.size()) {
                            return make_unexpected <error_type>("flc: memory surface too small");
                        }
                        std::memcpy(pixels.data(), fb_.data(), fb_.size());
                        return {};
                    }

                    if (!out.set_size(static_cast <int>(file_header_.width),
                                      static_cast <int>(file_header_.height),
                                      pixel_format::indexed8)) {
                        return make_unexpected <error_type>("flc: surface set_size failed");
                    }
                    out.set_palette_size(256);
                    out.write_palette(0, std::span <const std::uint8_t>{
                        palette_.data(), palette_.size()});
                    for (unsigned int y = 0; y < file_header_.height; ++y) {
                        out.write_pixels(
                            /*x_byte_off=*/0,
                            /*y=*/static_cast <int>(y),
                            /*count=*/static_cast <int>(file_header_.width),
                            fb_.data() + static_cast <std::size_t>(y) * fb_pitch_);
                    }
                    return {};
                }

                /**
                 * Walk top-level chunks starting at byte kFileHeaderSize,
                 * indexing those with FRAME_TYPE magic (0xF1FA). Any other
                 * chunk magic — recognised (PREFIX_TYPE, SEGMENT_TABLE,
                 * HUFFMAN_TABLE, SCRIPT) or unknown — is skipped past using
                 * its `size` field. This tolerates real-world files that
                 * carry vendor-specific chunks the FLIC spec doesn't list.
                 *
                 * Stops when the indexed-frame count reaches frame_count or
                 * when the stream is exhausted (whichever comes first).
                 */
                [[nodiscard]] result build_frame_index() {
                    if (stream_->seek(static_cast <std::int64_t>(flc::kFileHeaderSize),
                                      musac::seek_origin::set) < 0) {
                        return make_unexpected <error_type>("flc: cannot seek past file header");
                    }

                    frames_.clear();
                    frames_.reserve(file_header_.frame_count);

                    while (frames_.size() < file_header_.frame_count) {
                        const auto offset = stream_->tell();
                        if (offset < 0) {
                            return make_unexpected <error_type>("flc: tell failed");
                        }

                        // Just the 6-byte chunk preamble (size + magic). The
                        // remaining 10 bytes of a FRAME_TYPE header (sub_chunks,
                        // delay, etc.) are re-parsed in decode_frame().
                        std::array <std::uint8_t, 6> ch{};
                        const auto n = stream_->read(ch.data(), ch.size());
                        if (n == 0) {
                            // Stream ended before frame_count was reached. Don't
                            // fail — playback uses what we've indexed.
                            break;
                        }
                        if (n < ch.size()) {
                            return make_unexpected <error_type>("flc: truncated chunk header");
                        }

                        const std::uint32_t chunk_size  = bytes::read_u32le(ch.data());
                        const std::uint16_t chunk_magic = bytes::read_u16le(ch.data() + 4);
                        if (chunk_size < 6) {
                            return make_unexpected <error_type>("flc: chunk size smaller than header");
                        }

                        // 0xF1FA is the canonical frame magic. 0xF5FA appears
                        // in some real-world files (bit-flip variant?) with
                        // identical FRAME_TYPE structure, and ffmpeg treats it
                        // as a frame. Match that behavior for compatibility.
                        if (chunk_magic == flc::kFrameMagicStandard ||
                            chunk_magic == flc::kFrameMagicVariant) {
                            std::vector <std::uint8_t> frame_bytes(chunk_size);
                            std::memcpy(frame_bytes.data(), ch.data(), ch.size());
                            if (!read_exact(stream_, frame_bytes.data() + ch.size(),
                                            frame_bytes.size() - ch.size())) {
                                return make_unexpected <error_type>("flc: frame chunk truncated");
                            }
                            frames_.push_back({offset,
                                               chunk_size,
                                               compile_frame_chunks(frame_bytes)});
                        } else {
                            const auto body_skip = static_cast <std::int64_t>(chunk_size) - 6;
                            if (stream_->seek(body_skip, musac::seek_origin::cur) < 0) {
                                return make_unexpected <error_type>("flc: cannot seek past chunk body");
                            }
                        }
                    }
                    return {};
                }

                musac::io_stream*             stream_ = nullptr;
                flc::file_header              file_header_{};
                anim_info                     info_{};
                std::vector <frame_entry>     frames_;
                std::size_t                   cursor_ = 0;

                // Persistent decode state.
                std::vector <std::uint8_t>    fb_;             // indexed8 framebuffer
                std::size_t                   fb_pitch_ = 0;   // == width (no padding)
                std::array <std::uint8_t, 768> palette_{};     // 256 RGB triplets
                std::vector <std::uint8_t>    chunk_buf_;      // per-frame scratch
        };
    } // namespace

    std::unique_ptr <anim_decoder> flc_decoder::create() {
        return std::make_unique <flc_decoder_impl>();
    }
} // namespace onyx_anim
