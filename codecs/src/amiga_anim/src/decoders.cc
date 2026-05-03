#include <anim/decoders.hh>

#include <cstring>
#include <optional>
#include <string>
#include <type_traits>

namespace anim {
    expected <std::size_t>
    unpack_byterun1(std::span <const std::uint8_t> src,
                    std::uint8_t* dst, std::size_t expected) {
        std::size_t pi = 0;
        std::size_t si = 0;
        while (pi < expected) {
            if (si >= src.size()) {
                return make_unexpected("anim: ByteRun1 truncated at op");
            }
            if (const auto op = static_cast <std::int8_t>(src[si++]); op >= 0) {
                const std::size_t count = static_cast <std::size_t>(op) + 1u;
                if (si + count > src.size()) {
                    return make_unexpected("anim: ByteRun1 literal truncated");
                }
                if (pi + count > expected) {
                    return make_unexpected("anim: ByteRun1 literal overflows output");
                }
                std::memcpy(dst + pi, src.data() + si, count);
                si += count;
                pi += count;
            } else if (op != -128) {
                const std::size_t count = static_cast <std::size_t>(-op) + 1u;
                if (si >= src.size()) {
                    return make_unexpected("anim: ByteRun1 RLE truncated at data");
                }
                if (pi + count > expected) {
                    return make_unexpected("anim: ByteRun1 RLE overflows output");
                }
                std::memset(dst + pi, src[si++], count);
                pi += count;
            }
        }
        return si;
    }

    result
    apply_dlta_op5(std::span <const std::uint8_t> dlta,
                   std::uint8_t* fb,
                   std::size_t bytes_per_row,
                   unsigned int planes,
                   unsigned int height) {
        // 16 longword plane offsets at the head of the chunk; offset == 0
        // means "this plane is unchanged in this delta frame". The offsets
        // helper / `read_plane_offset` are defined further down in the
        // anonymous namespace; forward declarations live with byte_reader.
        constexpr std::size_t kOffsetTableBytes = 64;
        if (dlta.size() < kOffsetTableBytes) {
            return make_unexpected("anim: DLTA op5 too small for plane-offset table");
        }
        // Per-row stride in the row-interleaved framebuffer: bpr bytes for
        // each plane, all planes contiguous within the row.
        const std::size_t rowpitch = bytes_per_row * planes;

        bytes::byte_reader_be ptrs{dlta};
        for (unsigned int p = 0; p < planes && p < 16; ++p) {
            const auto off_raw = ptrs.read_u32();
            if (ptrs.failed()) {
                return make_unexpected("anim: DLTA op5 plane offset table truncated");
            }
            if (off_raw == 0) continue;
            if (off_raw >= dlta.size()) {
                return make_unexpected("anim: DLTA op5 plane offset past end");
            }

            bytes::byte_reader_be gb{dlta.subspan(off_raw)};

            for (std::size_t col = 0; col < bytes_per_row; ++col) {
                const auto op_count = gb.read_u8();
                if (gb.failed()) {
                    return make_unexpected("anim: DLTA op5 truncated at column header");
                }

                // Starting address of (row 0, plane p, column col) within the
                // row-interleaved framebuffer.
                std::size_t addr = static_cast <std::size_t>(p) * bytes_per_row + col;
                unsigned int row = 0;

                for (unsigned int i = 0; i < op_count; ++i) {
                    const auto op = gb.read_u8();
                    if (gb.failed()) {
                        return make_unexpected("anim: DLTA op5 truncated at op");
                    }

                    if (op == 0) {
                        // long SAME — overwrite next `count` rows with `value`
                        const auto count = gb.read_u8();
                        const auto v     = gb.read_u8();
                        if (gb.failed()) {
                            return make_unexpected("anim: DLTA op5 long-SAME truncated");
                        }
                        for (unsigned int r = 0; r < count; ++r) {
                            if (row >= height) break;
                            fb[addr] = v;
                            addr += rowpitch;
                            ++row;
                        }
                    } else if (op < 0x80u) {
                        // SKIP `op` rows — leave existing contents in place
                        const unsigned int n = op;
                        for (unsigned int r = 0; r < n && row < height; ++r) {
                            addr += rowpitch;
                            ++row;
                        }
                    } else {
                        // UNIQUE — overwrite next `count` rows with successive
                        // data bytes. (Op 5 is COPY semantics for animations,
                        // not XOR; the XOR mode is reserved for brush usage
                        // signaled by ANHD bits == 2. Matches ffmpeg's
                        // libavcodec/iff.c:decode_byte_vertical_delta.)
                        const unsigned int count = op & 0x7Fu;
                        if (!gb.has(count)) {
                            return make_unexpected("anim: DLTA op5 UNIQUE data truncated");
                        }
                        for (unsigned int r = 0; r < count; ++r) {
                            if (row >= height) {
                                gb >> bytes::skip(count - r);
                                break;
                            }
                            fb[addr] = gb.read_u8();
                            addr += rowpitch;
                            ++row;
                        }
                    }
                }
            }
        }
        return {};
    }

    void planar_interleaved_to_chunky(const std::uint8_t* planar,
                                      std::size_t bytes_per_row,
                                      unsigned int planes,
                                      std::uint8_t* chunky,
                                      std::size_t chunky_stride,
                                      unsigned int width,
                                      unsigned int height) noexcept {
        const std::size_t rowpitch = bytes_per_row * planes;
        for (unsigned int y = 0; y < height; ++y) {
            const std::uint8_t* src_row = planar + static_cast <std::size_t>(y) * rowpitch;
            std::uint8_t* dst_row = chunky + static_cast <std::size_t>(y) * chunky_stride;
            for (unsigned int x = 0; x < width; ++x) {
                const std::size_t byte_off = x >> 3u;
                const unsigned int bit = 7u - (x & 0x7u);
                std::uint8_t v = 0;
                for (unsigned int p = 0; p < planes; ++p) {
                    const std::uint8_t b = src_row[p * bytes_per_row + byte_off];
                    if (b & (1u << bit)) {
                        v = static_cast <std::uint8_t>(v | (1u << p));
                    }
                }
                dst_row[x] = v;
            }
        }
    }

    // ------------------------------------------------------------------------
    // Op 3 — Short Horizontal Delta
    //
    // Per plane, a stream of 16-bit BE records:
    //   offset >= 0: positive position advance (in 16-bit words from current
    //                pos), then write one 16-bit data word at the new pos.
    //   offset <  0 (and != 0xFFFF): position advance is `2 * -(offset + 2)`
    //                bytes, then a 16-bit count, then `count` 16-bit data
    //                words written at consecutive positions.
    //   offset == 0xFFFF: end-of-stream.
    // The framebuffer position is converted from "bytes within plane" to the
    // real offset in the row-interleaved buffer by:
    //   real = (pos / planepitch) * (planepitch * planes)
    //        + (pos % planepitch)
    //        + plane_index * planepitch
    // ------------------------------------------------------------------------
    result
    apply_dlta_op3(std::span <const std::uint8_t> dlta,
                   std::uint8_t* fb,
                   unsigned int width,
                   unsigned int planes,
                   std::size_t fb_size) {
        if (dlta.size() < static_cast <std::size_t>(planes) * 4u) {
            return make_unexpected("anim: DLTA op3 too small for plane-offset table");
        }
        const std::size_t planepitch = ((static_cast <std::size_t>(width) + 15u) / 16u) * 2u;
        const std::size_t pitch = planepitch * planes;

        bytes::byte_reader_be ptrs{dlta};
        for (unsigned int k = 0; k < planes; ++k) {
            const auto off_raw = ptrs.read_u32();
            if (ptrs.failed()) {
                return make_unexpected("anim: DLTA op3 plane offset table truncated");
            }
            if (off_raw == 0) continue;
            if (off_raw >= dlta.size()) continue;

            bytes::byte_reader_be gb{dlta.subspan(off_raw)};
            std::size_t pos = 0;
            while (true) {
                const auto raw = gb.read_u16();
                if (gb.failed()) return make_unexpected("anim: DLTA op3 truncated at offset");
                if (raw == 0xFFFF) break;
                const auto off16 = static_cast <std::int16_t>(raw);
                if (off16 >= 0) {
                    const auto data = gb.read_u16();
                    if (gb.failed()) return make_unexpected("anim: DLTA op3 truncated at single-data");
                    pos += static_cast <std::size_t>(off16) * 2u;
                    const std::size_t real = (pos / planepitch) * pitch +
                                             (pos % planepitch) +
                                             static_cast <std::size_t>(k) * planepitch;
                    if (real + 1 < fb_size) {
                        fb[real] = static_cast <std::uint8_t>(data >> 8);
                        fb[real + 1] = static_cast <std::uint8_t>(data & 0xFF);
                    }
                } else {
                    const auto count = gb.read_u16();
                    if (gb.failed()) return make_unexpected("anim: DLTA op3 truncated at run-count");
                    pos += static_cast <std::size_t>(2 * (-(off16 + 2)));
                    for (unsigned i = 0; i < count; ++i) {
                        const auto data = gb.read_u16();
                        if (gb.failed()) return make_unexpected("anim: DLTA op3 truncated at run data");
                        pos += 2u;
                        const std::size_t real = (pos / planepitch) * pitch +
                                                 (pos % planepitch) +
                                                 static_cast <std::size_t>(k) * planepitch;
                        if (real + 1 < fb_size) {
                            fb[real] = static_cast <std::uint8_t>(data >> 8);
                            fb[real + 1] = static_cast <std::uint8_t>(data & 0xFF);
                        }
                    }
                }
            }
        }
        return {};
    }

    namespace {
        // ------------------------------------------------------------------------
        // Read cursor — `bytes::byte_reader_be` from lib_bytes. The decoders
        // used to roll their own POD `cursor` with sentinel-returning rd_u8 /
        // rd_be16 / rd_be32 helpers; that's been replaced by the project-wide
        // byte_reader, which has the same sticky-failure semantics. After any
        // read sequence, check `c.failed()` (or `!c`) to detect truncation.
        // ------------------------------------------------------------------------
        using byte_reader = anim::byte_reader; // alias of bytes::byte_reader_be

        // Writes — byte_reader is read-only, so these stay as standalone
        // helpers. Each is a no-op if the write would exceed `cap`.
        inline void put_be16(std::uint8_t* fb, std::size_t off, std::size_t cap,
                             std::uint16_t v) {
            if (off + 1 >= cap) return;
            fb[off] = static_cast <std::uint8_t>(v >> 8);
            fb[off + 1] = static_cast <std::uint8_t>(v & 0xFF);
        }

        inline void put_be32(std::uint8_t* fb, std::size_t off, std::size_t cap,
                             std::uint32_t v) {
            if (off + 3 >= cap) return;
            fb[off] = static_cast <std::uint8_t>(v >> 24);
            fb[off + 1] = static_cast <std::uint8_t>(v >> 16);
            fb[off + 2] = static_cast <std::uint8_t>(v >> 8);
            fb[off + 3] = static_cast <std::uint8_t>(v & 0xFF);
        }

        // ------------------------------------------------------------------------
        // Plane geometry — the row-interleaved planar layout used by all delta
        // decoders. Each row holds plane 0's `planepitch_byte` bytes, then
        // plane 1's, ..., then plane (N-1)'s, with `pitch = planepitch * planes`
        // for stride. `planepitch` is rounded up to the nearest 16-bit word so
        // 16-bit writes never split a byte boundary.
        // ------------------------------------------------------------------------
        struct plane_geometry {
            std::size_t planepitch_byte; ///< (w + 7) / 8
            std::size_t planepitch;      ///< ((w + 15) / 16) * 2  (16-bit aligned)
            std::size_t pitch;           ///< planepitch * planes  (full row stride)

            static plane_geometry of(unsigned int width, unsigned int planes) noexcept {
                plane_geometry g{};
                g.planepitch_byte = (static_cast<std::size_t>(width) + 7u) / 8u;
                g.planepitch      = ((static_cast<std::size_t>(width) + 15u) / 16u) * 2u;
                g.pitch           = g.planepitch * planes;
                return g;
            }

            // Translate a per-plane byte offset to a byte address in the
            // row-interleaved framebuffer at plane `k`.
            std::size_t addr_in_fb(std::size_t off_in_plane,
                                   unsigned int k) const noexcept {
                return (off_in_plane / planepitch_byte) * pitch +
                       (off_in_plane % planepitch_byte) +
                       static_cast<std::size_t>(k) * planepitch;
            }
        };

        // Read the next per-plane offset from the offset table.
        // Returns:
        //   non-empty optional → byte offset to the plane's stream (validated
        //                        to be in-range against the reader's full
        //                        span); zero yields std::nullopt ("skip plane").
        //   error              → table truncated.
        // The "skip-on-zero / skip-on-out-of-range" behaviour mirrors ffmpeg's
        // libavcodec/iff.c, which silently skips planes with bogus offsets.
        inline expected<std::optional<std::size_t>>
        read_plane_offset(byte_reader& ptrs) {
            const auto raw = ptrs.read_u32();
            if (ptrs.failed()) {
                return make_unexpected("anim: plane offset table truncated");
            }
            if (raw == 0) return std::optional<std::size_t>{};
            const auto off = static_cast<std::size_t>(raw);
            if (off >= ptrs.size()) return std::optional<std::size_t>{};
            return std::optional<std::size_t>{off};
        }
    } // namespace

    // ------------------------------------------------------------------------
    // Op 7 short — 16-bit Vertical Delta (`decode_short_vertical_delta`).
    //
    // Header has TWO offset tables of 8 longwords each (32 bytes per table,
    // 64 bytes total): the first gives per-plane *opcode* offsets, the second
    // gives per-plane *data* offsets. Each plane reads opcodes and 16-bit data
    // from independent positions within the chunk.
    //
    // Per column, op_count byte then op bytes (same encoding as op 5 but
    // every emit writes a 16-bit BE word instead of a byte).
    // ------------------------------------------------------------------------
    result
    apply_dlta_op7_short(std::span <const std::uint8_t> dlta,
                         std::uint8_t* fb,
                         unsigned int width,
                         unsigned int planes,
                         std::size_t fb_size) {
        if (dlta.size() <= 64) {
            return make_unexpected("anim: DLTA op7-short too small");
        }
        const unsigned ncolumns = (width + 15u) >> 4u;
        const std::size_t dstpitch = static_cast <std::size_t>(ncolumns) * planes * 2u;

        byte_reader ptrs{dlta};                      // opcode-offset table at 0..31
        byte_reader dptrs{dlta.subspan(32)};         // data-offset   table at 32..63

        for (unsigned int k = 0; k < planes; ++k) {
            const auto ofssrc  = ptrs.read_u32();
            const auto ofsdata = dptrs.read_u32();
            if (ptrs.failed() || dptrs.failed()) {
                return make_unexpected("anim: DLTA op7-short header truncated");
            }
            if (ofssrc == 0) continue;
            if (ofssrc >= dlta.size() || ofsdata >= dlta.size()) {
                continue; // matches ffmpeg: silently skip planes with bogus offsets
            }
            byte_reader gb{dlta.subspan(ofssrc)};
            byte_reader dgb{dlta.subspan(ofsdata)};

            for (unsigned int j = 0; j < ncolumns; ++j) {
                std::size_t ofsdst = (static_cast <std::size_t>(j) +
                                      static_cast <std::size_t>(k) * ncolumns) * 2u;
                const auto op_count = gb.read_u8();
                if (gb.failed()) return make_unexpected("anim: op7-short col header truncated");
                int i = op_count;
                while (i > 0) {
                    const auto op = gb.read_u8();
                    if (gb.failed()) return make_unexpected("anim: op7-short op truncated");
                    if (op == 0) {
                        const auto count = gb.read_u8();
                        const auto x     = dgb.read_u16();
                        if (gb.failed() || dgb.failed()) {
                            return make_unexpected("anim: op7-short SAME truncated");
                        }
                        for (int n = 0; n < count; ++n) {
                            put_be16(fb, ofsdst, fb_size, x);
                            ofsdst += dstpitch;
                        }
                    } else if (op < 0x80) {
                        ofsdst += static_cast <std::size_t>(op) * dstpitch;
                    } else {
                        const int count = op & 0x7F;
                        for (int n = 0; n < count; ++n) {
                            const auto x = dgb.read_u16();
                            if (dgb.failed()) return make_unexpected("anim: op7-short UNIQUE truncated");
                            put_be16(fb, ofsdst, fb_size, x);
                            ofsdst += dstpitch;
                        }
                    }
                    --i;
                }
            }
        }
        return {};
    }

    // ------------------------------------------------------------------------
    // Op 7 long — 32-bit Vertical Delta. Same shape as op 7 short but each
    // emit writes a 32-bit BE longword. Includes ffmpeg's "h" kludge for
    // widths that aren't aligned to 32 — for the last column of such widths,
    // the 16-bit version of the write is used.
    // ------------------------------------------------------------------------
    result
    apply_dlta_op7_long(std::span <const std::uint8_t> dlta,
                        std::uint8_t* fb,
                        unsigned int width,
                        unsigned int planes,
                        std::size_t fb_size) {
        if (dlta.size() <= 64) {
            return make_unexpected("anim: DLTA op7-long too small");
        }
        const unsigned ncolumns = (width + 31u) >> 5u;
        const std::size_t dstpitch = (((static_cast <std::size_t>(width) + 15u) / 16u) * 2u) * planes;
        const bool h = ((((width + 15u) / 16u) * 2u) !=
                        (((width + 31u) / 32u) * 4u));

        byte_reader ptrs{dlta};
        byte_reader dptrs{dlta.subspan(32)};

        for (unsigned int k = 0; k < planes; ++k) {
            const auto ofssrc  = ptrs.read_u32();
            const auto ofsdata = dptrs.read_u32();
            if (ptrs.failed() || dptrs.failed()) {
                return make_unexpected("anim: DLTA op7-long header truncated");
            }
            if (ofssrc == 0) continue;
            if (ofssrc >= dlta.size() || ofsdata >= dlta.size()) continue;
            byte_reader gb{dlta.subspan(ofssrc)};
            byte_reader dgb{dlta.subspan(ofsdata)};

            for (unsigned int j = 0; j < ncolumns; ++j) {
                auto ofsdst = static_cast <std::int64_t>(
                    (static_cast <std::size_t>(j) +
                     static_cast <std::size_t>(k) * ncolumns) * 4u);
                ofsdst -= static_cast <std::int64_t>(h ? (2u * k) : 0u);
                const bool last_kludge = h && (j == (ncolumns - 1));
                const auto op_count = gb.read_u8();
                if (gb.failed()) return make_unexpected("anim: op7-long col header truncated");
                int i = op_count;
                while (i > 0) {
                    const auto op = gb.read_u8();
                    if (gb.failed()) return make_unexpected("anim: op7-long op truncated");
                    if (op == 0) {
                        const auto count = gb.read_u8();
                        if (gb.failed()) return make_unexpected("anim: op7-long SAME count truncated");
                        std::uint32_t x;
                        if (last_kludge) {
                            x = dgb.read_u16();
                            (void)dgb.read_u16(); // skip 2 bytes
                            if (dgb.failed()) return make_unexpected("anim: op7-long SAME data truncated");
                        } else {
                            x = dgb.read_u32();
                            if (dgb.failed()) return make_unexpected("anim: op7-long SAME data truncated");
                        }
                        for (int n = 0; n < count; ++n) {
                            if (ofsdst < 0) {
                                ofsdst += static_cast <std::int64_t>(dstpitch);
                                continue;
                            }
                            if (last_kludge) {
                                put_be16(fb, static_cast <std::size_t>(ofsdst), fb_size,
                                         static_cast <std::uint16_t>(x));
                            } else {
                                put_be32(fb, static_cast <std::size_t>(ofsdst), fb_size, x);
                            }
                            ofsdst += static_cast <std::int64_t>(dstpitch);
                        }
                    } else if (op < 0x80) {
                        ofsdst += static_cast <std::int64_t>(op) * static_cast <std::int64_t>(dstpitch);
                    } else {
                        const int count = op & 0x7F;
                        for (int n = 0; n < count; ++n) {
                            if (last_kludge) {
                                const auto v = dgb.read_u16();
                                if (dgb.failed()) return make_unexpected("anim: op7-long UNIQUE data truncated");
                                if (ofsdst >= 0) {
                                    put_be16(fb, static_cast <std::size_t>(ofsdst), fb_size, v);
                                }
                                (void)dgb.read_u16();
                            } else {
                                const auto v = dgb.read_u32();
                                if (dgb.failed()) return make_unexpected("anim: op7-long UNIQUE data truncated");
                                if (ofsdst >= 0) {
                                    put_be32(fb, static_cast <std::size_t>(ofsdst), fb_size, v);
                                }
                            }
                            ofsdst += static_cast <std::int64_t>(dstpitch);
                        }
                    }
                    --i;
                }
            }
        }
        return {};
    }

    // ------------------------------------------------------------------------
    // Op 8 short — 16-bit Vertical Delta v2. Like op 7 short but ops AND
    // data are interleaved in one stream per plane (no separate data table),
    // and opcodes themselves are 16-bit BE.
    // ------------------------------------------------------------------------
    result
    apply_dlta_op8_short(std::span <const std::uint8_t> dlta,
                         std::uint8_t* fb,
                         unsigned int width,
                         unsigned int planes,
                         std::size_t fb_size) {
        if (dlta.size() < 64) {
            return make_unexpected("anim: DLTA op8-short too small");
        }
        const unsigned ncolumns = (width + 15u) >> 4u;
        const std::size_t dstpitch = static_cast <std::size_t>(ncolumns) * planes * 2u;

        byte_reader ptrs{dlta};

        for (unsigned int k = 0; k < planes; ++k) {
            const auto ofssrc = ptrs.read_u32();
            if (ptrs.failed()) return make_unexpected("anim: DLTA op8-short header truncated");
            if (ofssrc == 0) continue;
            if (ofssrc >= dlta.size()) continue;
            byte_reader gb{dlta.subspan(ofssrc)};

            for (unsigned int j = 0; j < ncolumns; ++j) {
                std::size_t ofsdst = (static_cast <std::size_t>(j) +
                                      static_cast <std::size_t>(k) * ncolumns) * 2u;
                const auto i_init = gb.read_u16();
                if (gb.failed()) return make_unexpected("anim: op8-short col header truncated");
                int i = i_init;
                while (i > 0 && gb.remaining() > 4u) {
                    const auto op = gb.read_u16();
                    if (gb.failed()) return make_unexpected("anim: op8-short op truncated");
                    if (op == 0) {
                        const auto count = gb.read_u16();
                        const auto x     = gb.read_u16();
                        if (gb.failed()) {
                            return make_unexpected("anim: op8-short SAME truncated");
                        }
                        for (int n = 0; n < count; ++n) {
                            put_be16(fb, ofsdst, fb_size, x);
                            ofsdst += dstpitch;
                        }
                    } else if (op < 0x8000) {
                        ofsdst += static_cast <std::size_t>(op) * dstpitch;
                    } else {
                        const int count = op & 0x7FFF;
                        for (int n = 0; n < count; ++n) {
                            const auto x = gb.read_u16();
                            if (gb.failed()) return make_unexpected("anim: op8-short UNIQUE truncated");
                            put_be16(fb, ofsdst, fb_size, x);
                            ofsdst += dstpitch;
                        }
                    }
                    --i;
                }
            }
        }
        return {};
    }

    // ------------------------------------------------------------------------
    // Op 8 long — 32-bit variant of op 8 short. Opcode/count are 32-bit BE.
    // Includes the same "h" last-column-uses-16-bit kludge as op 7 long.
    // ------------------------------------------------------------------------
    result
    apply_dlta_op8_long(std::span <const std::uint8_t> dlta,
                        std::uint8_t* fb,
                        unsigned int width,
                        unsigned int planes,
                        std::size_t fb_size) {
        if (dlta.size() < 64) {
            return make_unexpected("anim: DLTA op8-long too small");
        }
        const unsigned ncolumns = (width + 31u) >> 5u;
        const std::size_t dstpitch = (((static_cast <std::size_t>(width) + 15u) / 16u) * 2u) * planes;
        const bool h = ((((width + 15u) / 16u) * 2u) !=
                        (((width + 31u) / 32u) * 4u));

        byte_reader ptrs{dlta};

        for (unsigned int k = 0; k < planes; ++k) {
            const auto ofssrc = ptrs.read_u32();
            if (ptrs.failed()) return make_unexpected("anim: DLTA op8-long header truncated");
            if (ofssrc == 0) continue;
            if (ofssrc >= dlta.size()) continue;
            byte_reader gb{dlta.subspan(ofssrc)};

            for (unsigned int j = 0; j < ncolumns; ++j) {
                auto ofsdst = static_cast <std::int64_t>(
                    (static_cast <std::size_t>(j) +
                     static_cast <std::size_t>(k) * ncolumns) * 4u);
                ofsdst -= static_cast <std::int64_t>(h ? (2u * k) : 0u);
                const bool last_kludge = h && (j == (ncolumns - 1));
                const std::uint32_t skip_mask = last_kludge ? 0x8000u : 0x80000000u;
                const std::uint32_t value_mask = skip_mask - 1u;

                const auto i_init = gb.read_u32();
                if (gb.failed()) return make_unexpected("anim: op8-long col header truncated");
                std::int64_t i = i_init;
                while (i > 0 && gb.remaining() > 4u) {
                    const auto op = gb.read_u32();
                    if (gb.failed()) return make_unexpected("anim: op8-long op truncated");
                    if (op == 0) {
                        std::uint32_t count, x;
                        if (last_kludge) {
                            count = gb.read_u16();
                            x     = gb.read_u16();
                        } else {
                            count = gb.read_u32();
                            x     = gb.read_u32();
                        }
                        if (gb.failed()) return make_unexpected("anim: op8-long SAME truncated");
                        for (std::uint32_t n = 0; n < count; ++n) {
                            if (ofsdst >= 0) {
                                if (last_kludge) {
                                    put_be16(fb, static_cast <std::size_t>(ofsdst), fb_size,
                                             static_cast <std::uint16_t>(x));
                                } else {
                                    put_be32(fb, static_cast <std::size_t>(ofsdst), fb_size, x);
                                }
                            }
                            ofsdst += static_cast <std::int64_t>(dstpitch);
                        }
                    } else if (op < skip_mask) {
                        ofsdst += static_cast <std::int64_t>(op) * static_cast <std::int64_t>(dstpitch);
                    } else {
                        std::uint32_t count = op & value_mask;
                        for (std::uint32_t n = 0; n < count; ++n) {
                            if (last_kludge) {
                                const auto v = gb.read_u16();
                                if (gb.failed()) return make_unexpected("anim: op8-long UNIQUE truncated");
                                if (ofsdst >= 0) {
                                    put_be16(fb, static_cast <std::size_t>(ofsdst), fb_size, v);
                                }
                            } else {
                                const auto v = gb.read_u32();
                                if (gb.failed()) return make_unexpected("anim: op8-long UNIQUE truncated");
                                if (ofsdst >= 0) {
                                    put_be32(fb, static_cast <std::size_t>(ofsdst), fb_size, v);
                                }
                            }
                            ofsdst += static_cast <std::int64_t>(dstpitch);
                        }
                    }
                    --i;
                }
            }
        }
        return {};
    }

    // ------------------------------------------------------------------------
    // Op J — type-tagged record stream. Each record begins with a 16-bit
    // BE `type`:
    //   0: end-of-stream
    //   1: column-group write (flag, cols, groups, then per-group offset and
    //      data; flag toggles XOR vs COPY)
    //   2: row-group write (flag, rows, bytes, groups, ...)
    // The `kludge_j` factor handles widths smaller than 320 by adjusting the
    // base offset to align with a 320-pixel-wide canvas.
    // ------------------------------------------------------------------------
    result
    apply_dlta_opj(std::span <const std::uint8_t> dlta,
                   std::uint8_t* fb,
                   unsigned int width,
                   unsigned int /*height*/,
                   unsigned int planes,
                   std::size_t fb_size) {
        const auto geom = plane_geometry::of(width, planes);
        const std::size_t planepitch_byte = geom.planepitch_byte;
        const std::size_t planepitch     = geom.planepitch;
        const std::size_t pitch          = geom.pitch;
        const std::size_t kludge_j = (width < 320u) ? ((320u - width) / 8u / 2u) : 0u;

        byte_reader gb{dlta};

        auto translate_offset = [&](std::uint32_t off) -> std::int64_t {
            if (kludge_j) {
                return static_cast <std::int64_t>((off / (320u / 8u)) * pitch +
                                                  (off % (320u / 8u))) -
                       static_cast <std::int64_t>(kludge_j);
            }
            return static_cast <std::int64_t>((off / planepitch_byte) * pitch +
                                              (off % planepitch_byte));
        };

        while (gb.remaining() >= 2u) {
            const auto type = gb.read_u16();
            if (gb.failed()) return make_unexpected("anim: opJ truncated at type");
            switch (type) {
                case 0:
                    return {};
                case 1: {
                    const auto flag   = gb.read_u16();
                    const auto cols   = gb.read_u16();
                    const auto groups = gb.read_u16();
                    if (gb.failed()) {
                        return make_unexpected("anim: opJ type-1 header truncated");
                    }
                    for (int g = 0; g < groups; ++g) {
                        const auto raw_off = gb.read_u16();
                        if (gb.failed()) {
                            return make_unexpected("anim: opJ type-1 group offset truncated");
                        }
                        if (static_cast <std::size_t>(cols) * planes == 0 ||
                            gb.remaining() < static_cast <std::size_t>(cols) * planes) {
                            return make_unexpected("anim: opJ type-1 cols*planes invalid");
                        }
                        std::int64_t offset = translate_offset(static_cast <std::uint32_t>(raw_off));
                        for (int b = 0; b < cols; ++b) {
                            for (unsigned d = 0; d < planes; ++d) {
                                const auto value = gb.read_u8();
                                if (gb.failed()) {
                                    return make_unexpected("anim: opJ type-1 data truncated");
                                }
                                if (offset >= 0 &&
                                    static_cast <std::size_t>(offset) < fb_size) {
                                    if (flag) {
                                        fb[offset] = static_cast <std::uint8_t>(fb[offset] ^ value);
                                    } else {
                                        fb[offset] = value;
                                    }
                                }
                                offset += static_cast <std::int64_t>(planepitch);
                            }
                        }
                        if ((static_cast <std::size_t>(cols) * planes) & 1u) {
                            (void)gb.read_u8(); // alignment padding
                        }
                    }
                    break;
                }
                case 2: {
                    const auto flag   = gb.read_u16();
                    const auto rows   = gb.read_u16();
                    const auto bytes  = gb.read_u16();
                    const auto groups = gb.read_u16();
                    if (gb.failed()) {
                        return make_unexpected("anim: opJ type-2 header truncated");
                    }
                    for (int g = 0; g < groups; ++g) {
                        const auto raw_off = gb.read_u16();
                        if (gb.failed()) {
                            return make_unexpected("anim: opJ type-2 group offset truncated");
                        }
                        std::int64_t offset = translate_offset(static_cast <std::uint32_t>(raw_off));
                        for (int r = 0; r < rows; ++r) {
                            for (unsigned d = 0; d < planes; ++d) {
                                std::int64_t noffset = offset +
                                                       static_cast <std::int64_t>(r) * static_cast <std::int64_t>(pitch)
                                                       +
                                                       static_cast <std::int64_t>(d) * static_cast <std::int64_t>(
                                                           planepitch);
                                if (bytes == 0 || gb.remaining() < static_cast <std::size_t>(bytes)) {
                                    return make_unexpected("anim: opJ type-2 bytes invalid");
                                }
                                for (int b = 0; b < bytes; ++b) {
                                    const auto value = gb.read_u8();
                                    if (gb.failed()) {
                                        return make_unexpected("anim: opJ type-2 data truncated");
                                    }
                                    if (noffset >= 0 &&
                                        static_cast <std::size_t>(noffset) < fb_size) {
                                        if (flag) {
                                            fb[noffset] = static_cast <std::uint8_t>(fb[noffset] ^ value);
                                        } else {
                                            fb[noffset] = value;
                                        }
                                    }
                                    ++noffset;
                                }
                            }
                        }
                        if ((static_cast <std::size_t>(rows) * static_cast <std::size_t>(bytes) * planes) & 1u) {
                            (void)gb.read_u8();
                        }
                    }
                    break;
                }
                default:
                    return {};
            }
        }
        return {};
    }

    // ------------------------------------------------------------------------
    // Op L (0x6C / 108). Mirrors ffmpeg's `decode_delta_l` in libavcodec/iff.c.
    //
    // Header (64 bytes):
    //   bytes  0..31 : 8 longwords — per-plane *data* word-offsets
    //   bytes 32..63 : 8 longwords — per-plane *opcode* word-offsets
    // Both offsets are multiplied by 2 to get byte positions within the chunk.
    //
    // Per plane k (where data offset != 0): walk the opcode stream as a
    // sequence of (offset_w, count_w) BE16 pairs. Stream ends when the next
    // peeked u16 is 0xFFFF. Each record:
    //   offset_w → byte position within plane k of the first 16-bit write
    //   count_w  s16:
    //     < 0 : read one BE16 datum, write |cnt| copies stepping by `dstpitch`
    //     > 0 : read `cnt` BE16 data words, writing each at offset stepping by
    //           `dstpitch`
    //
    // Write stride:
    //   is_short == true  : dstpitch = planepitch_byte * planes
    //                       (full row stride → vertical run within plane k)
    //   is_short == false : dstpitch = 2  (consecutive 16-bit words →
    //                       horizontal run starting at offset)
    //
    // The per-plane `offset_w` is in plane-local coordinates; convert to a
    // byte address in the row-interleaved framebuffer with:
    //   row    = (2*off) / planepitch_byte
    //   colbyt = (2*off) % planepitch_byte
    //   addr   = row * pitch + colbyt + k * planepitch
    // ------------------------------------------------------------------------
    result
    apply_dlta_op_l(std::span <const std::uint8_t> dlta,
                    std::uint8_t* fb,
                    unsigned int width,
                    unsigned int planes,
                    bool is_short,
                    std::size_t fb_size) {
        if (dlta.size() <= 64) {
            return make_unexpected("anim: DLTA opL too small");
        }
        const auto geom = plane_geometry::of(width, planes);
        if (geom.planepitch_byte == 0) {
            return make_unexpected("anim: opL zero plane pitch");
        }
        const std::size_t dstpitch = is_short ? (geom.planepitch_byte * planes) : 2u;

        // dptrs reads data offsets (table at 0); optrs reads opcode offsets
        // (table at 32). Naming mirrors ffmpeg's poff0/poff1.
        byte_reader dptrs{dlta};
        byte_reader optrs{dlta.subspan(32)};

        for (unsigned int k = 0; k < planes; ++k) {
            // Op L's offsets are word offsets (×2 to byte position), unlike
            // op D/E. We can't reuse read_plane_offset directly — it operates
            // in byte units and would reject offsets > cap/2 as out-of-range.
            const auto poff_data = dptrs.read_u32();
            const auto poff_ops  = optrs.read_u32();
            if (dptrs.failed() || optrs.failed()) {
                return make_unexpected("anim: opL plane offset table truncated");
            }
            if (poff_data == 0) continue;
            const std::size_t data_pos = static_cast<std::size_t>(poff_data) * 2u;
            const std::size_t ops_pos  = static_cast<std::size_t>(poff_ops)  * 2u;
            if (data_pos >= dlta.size() || ops_pos >= dlta.size()) continue;

            byte_reader dgb{dlta.subspan(data_pos)};
            byte_reader ogb{dlta.subspan(ops_pos)};

            // Walk records until the terminator (peek == 0xFFFF) or stream end.
            for (;;) {
                if (ogb.remaining() < 4u) break;
                const auto peek = bytes::read_u16be(ogb.peek());
                if (peek == 0xFFFFu) break;

                const auto raw_off = ogb.read_u16();
                const auto cnt     = ogb.read_s16();
                if (ogb.failed()) {
                    return make_unexpected("anim: opL record truncated");
                }
                std::size_t addr = geom.addr_in_fb(
                    static_cast<std::size_t>(raw_off) * 2u, k);

                if (cnt < 0) {
                    const auto data = dgb.read_u16();
                    if (dgb.failed()) return make_unexpected("anim: opL run datum truncated");
                    const int reps = -cnt;
                    for (int i = 0; i < reps; ++i) {
                        put_be16(fb, addr, fb_size, data);
                        addr += dstpitch;
                    }
                } else {
                    for (int i = 0; i < cnt; ++i) {
                        const auto data = dgb.read_u16();
                        if (dgb.failed()) {
                            return make_unexpected("anim: opL literal data truncated");
                        }
                        put_be16(fb, addr, fb_size, data);
                        addr += dstpitch;
                    }
                }
            }
        }
        return {};
    }

    namespace {
        // ------------------------------------------------------------------------
        // Op D / Op E shared decoder — Scala/InfoChannel ANIM32 (op D, 100,
        // 0x64) and ANIM16 (op E, 101, 0x65). Mirrors ffmpeg's
        // `decode_delta_d` / `decode_delta_e` (libavcodec/iff.c).
        //
        // Same algorithm in both; only the integer widths differ:
        //   op D: u32 entries, s32 opcode, u32 data, 8-byte record
        //   op E: u16 entries, s16 opcode, u16 data, 6-byte record
        // The offset is u32 either way. Writes step by full row pitch.
        //
        // Header: `planes` longwords — per-plane byte offsets into the chunk
        // where that plane's stream lives. Per plane:
        //   <Entries>            entries
        //   entries × (<Opcode> opcode, u32 offset)
        //     opcode >= 0 : read one <Data> datum, write `opcode` copies
        //                   stepping by `pitch`
        //     opcode <  0 : read |opcode| <Data> literals, each stepping pitch
        // ------------------------------------------------------------------------
        struct anim_d {
            using Entries = std::uint32_t;
            using Opcode  = std::int32_t;
            using Data    = std::uint32_t;
            static constexpr std::size_t record_size = 8u;
            static constexpr std::size_t data_size   = 4u;
            static constexpr const char* tag         = "opD";
        };
        struct anim_e {
            using Entries = std::uint16_t;
            using Opcode  = std::int16_t;
            using Data    = std::uint16_t;
            static constexpr std::size_t record_size = 6u;
            static constexpr std::size_t data_size   = 2u;
            static constexpr const char* tag         = "opE";
        };

        // Reads a Scala-style integer (u16 or u32, both BE) via byte_reader.
        // Returns the value as u32; failure is reported via the reader's
        // sticky `failed()` flag (caller checks).
        template <typename T>
        inline std::uint32_t scala_read(byte_reader& c) {
            if constexpr (sizeof(T) == 2) return c.read_u16();
            else                          return c.read_u32();
        }

        template <typename T>
        inline void scala_put(std::uint8_t* fb, std::size_t off,
                              std::size_t cap, std::uint32_t v) {
            if constexpr (sizeof(T) == 2) {
                put_be16(fb, off, cap, static_cast<std::uint16_t>(v));
            } else {
                put_be32(fb, off, cap, v);
            }
        }

        template <typename V>
        result decode_scala_anim(std::span<const std::uint8_t> dlta,
                                 std::uint8_t* fb,
                                 unsigned int width,
                                 unsigned int planes,
                                 std::size_t  fb_size) {
            using Entries = typename V::Entries;
            using Opcode  = typename V::Opcode;
            using Data    = typename V::Data;

            if (planes == 0) return {};
            if (dlta.size() <= static_cast<std::size_t>(planes) * 4u) {
                return make_unexpected(std::string("anim: DLTA ") + V::tag + " too small");
            }
            const auto geom = plane_geometry::of(width, planes);
            if (geom.planepitch_byte == 0) {
                return make_unexpected(std::string("anim: ") + V::tag + " zero plane pitch");
            }

            byte_reader ptrs{dlta};

            for (unsigned int k = 0; k < planes; ++k) {
                auto plane_off = read_plane_offset(ptrs);
                if (!plane_off) return make_unexpected(plane_off.error());
                if (!plane_off->has_value()) continue;

                byte_reader gb{dlta.subspan(**plane_off)};
                const auto entries_raw = scala_read<Entries>(gb);
                if (gb.failed()) {
                    return make_unexpected(std::string("anim: ") + V::tag +
                                           " entries header truncated");
                }
                auto entries = static_cast<Entries>(entries_raw);

                // Each record is a fixed header (opcode + offset); payloads
                // beyond that are bounded by entries × record_size up front
                // for op D, but op E doesn't pre-check (matches ffmpeg).
                if constexpr (std::is_same_v<V, anim_d>) {
                    if (static_cast<std::size_t>(entries) * V::record_size >
                        gb.remaining()) {
                        return make_unexpected(std::string("anim: ") + V::tag +
                                               " entries count exceeds chunk");
                    }
                }

                while (entries && gb.remaining() >= V::record_size) {
                    const auto op_raw  = scala_read<Entries>(gb); // opcode shares width with entries
                    const auto off_raw = gb.read_u32();
                    if (gb.failed()) {
                        return make_unexpected(std::string("anim: ") + V::tag +
                                               " record truncated");
                    }
                    const auto opcode = static_cast<Opcode>(static_cast<Entries>(op_raw));
                    std::size_t addr = geom.addr_in_fb(
                        static_cast<std::size_t>(off_raw), k);

                    if (opcode >= 0) {
                        if (gb.remaining() < V::data_size) {
                            return make_unexpected(std::string("anim: ") + V::tag +
                                                   " run datum truncated");
                        }
                        const auto x = scala_read<Data>(gb);
                        if (gb.failed()) {
                            return make_unexpected(std::string("anim: ") + V::tag +
                                                   " run datum truncated");
                        }
                        // Mirror ffmpeg's op-D sanity check: skip records
                        // whose run would overflow the destination starting
                        // from `addr`. Op E has no equivalent guard.
                        if constexpr (std::is_same_v<V, anim_d>) {
                            if (opcode > 0 &&
                                addr + V::data_size +
                                    static_cast<std::size_t>(opcode - 1) * geom.pitch >
                                    fb_size) {
                                --entries;
                                continue;
                            }
                        }
                        Opcode n = opcode;
                        while (n > 0 && addr < fb_size) {
                            scala_put<Data>(fb, addr, fb_size, x);
                            addr += geom.pitch;
                            --n;
                        }
                    } else {
                        auto n = static_cast<Opcode>(-opcode);
                        while (n > 0 && gb.remaining() >= V::data_size) {
                            const auto v = scala_read<Data>(gb);
                            if (gb.failed()) {
                                return make_unexpected(std::string("anim: ") + V::tag +
                                                       " literal truncated");
                            }
                            if (addr < fb_size) {
                                scala_put<Data>(fb, addr, fb_size, v);
                            }
                            addr += geom.pitch;
                            --n;
                        }
                    }
                    --entries;
                }
            }
            return {};
        }
    } // namespace

    result
    apply_dlta_op_d(std::span<const std::uint8_t> dlta,
                    std::uint8_t* fb,
                    unsigned int  width,
                    unsigned int  planes,
                    std::size_t   fb_size) {
        return decode_scala_anim<anim_d>(dlta, fb, width, planes, fb_size);
    }

    result
    apply_dlta_op_e(std::span<const std::uint8_t> dlta,
                    std::uint8_t* fb,
                    unsigned int  width,
                    unsigned int  planes,
                    std::size_t   fb_size) {
        return decode_scala_anim<anim_e>(dlta, fb, width, planes, fb_size);
    }
} // namespace anim
