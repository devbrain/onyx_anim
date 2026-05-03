#include <anim/decoders.hh>

#include <cstring>

namespace anim {
    expected<std::size_t>
    unpack_byterun1(std::span<const std::uint8_t> src,
                    std::uint8_t* dst, std::size_t expected) {
        std::size_t pi = 0;
        std::size_t si = 0;
        while (pi < expected) {
            if (si >= src.size()) {
                return make_unexpected("anim: ByteRun1 truncated at op");
            }
            const auto op = static_cast<std::int8_t>(src[si++]);
            if (op >= 0) {
                const std::size_t count = static_cast<std::size_t>(op) + 1u;
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
                const std::size_t count = static_cast<std::size_t>(-op) + 1u;
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

    namespace {
        std::uint32_t r32be(const std::uint8_t* p) noexcept {
            return (static_cast<std::uint32_t>(p[0]) << 24u) |
                   (static_cast<std::uint32_t>(p[1]) << 16u) |
                   (static_cast<std::uint32_t>(p[2]) <<  8u) |
                    static_cast<std::uint32_t>(p[3]);
        }
    } // namespace

    result
    apply_dlta_op5(std::span<const std::uint8_t> dlta,
                   std::uint8_t* fb,
                   std::size_t bytes_per_row,
                   unsigned int planes,
                   unsigned int height) {
        // 16 longword plane offsets at the head of the chunk; offset == 0
        // means "this plane is unchanged in this delta frame".
        constexpr std::size_t kOffsetTableBytes = 64;
        if (dlta.size() < kOffsetTableBytes) {
            return make_unexpected("anim: DLTA op5 too small for plane-offset table");
        }
        const std::size_t cap      = dlta.size();
        const auto*       base     = dlta.data();
        // Per-row stride in the row-interleaved framebuffer: bpr bytes for
        // each plane, all planes contiguous within the row.
        const std::size_t rowpitch = bytes_per_row * planes;

        for (unsigned int p = 0; p < planes && p < 16; ++p) {
            const std::uint32_t off = r32be(base + p * 4u);
            if (off == 0) continue;
            if (off >= cap) {
                return make_unexpected("anim: DLTA op5 plane offset past end");
            }

            std::size_t si = off;

            for (std::size_t col = 0; col < bytes_per_row; ++col) {
                if (si >= cap) {
                    return make_unexpected("anim: DLTA op5 truncated at column header");
                }
                const std::uint8_t op_count = base[si++];

                // Starting address of (row 0, plane p, column col) within the
                // row-interleaved framebuffer.
                std::size_t addr = static_cast<std::size_t>(p) * bytes_per_row + col;
                unsigned int row = 0;

                for (unsigned int i = 0; i < op_count; ++i) {
                    if (si >= cap) {
                        return make_unexpected("anim: DLTA op5 truncated at op");
                    }
                    const std::uint8_t op = base[si++];

                    if (op == 0) {
                        // long SAME — overwrite next `count` rows with `value`
                        if (si + 1 >= cap) {
                            return make_unexpected("anim: DLTA op5 long-SAME truncated");
                        }
                        const unsigned int count = base[si++];
                        const std::uint8_t   v   = base[si++];
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
                        if (si + count > cap) {
                            return make_unexpected("anim: DLTA op5 UNIQUE data truncated");
                        }
                        for (unsigned int r = 0; r < count; ++r) {
                            if (row >= height) {
                                si += (count - r);
                                break;
                            }
                            fb[addr] = base[si++];
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
            const std::uint8_t* src_row = planar + static_cast<std::size_t>(y) * rowpitch;
            std::uint8_t*       dst_row = chunky + static_cast<std::size_t>(y) * chunky_stride;
            for (unsigned int x = 0; x < width; ++x) {
                const std::size_t byte_off = x >> 3u;
                const unsigned int bit     = 7u - (x & 0x7u);
                std::uint8_t v = 0;
                for (unsigned int p = 0; p < planes; ++p) {
                    const std::uint8_t b = src_row[p * bytes_per_row + byte_off];
                    if (b & (1u << bit)) {
                        v = static_cast<std::uint8_t>(v | (1u << p));
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
    apply_dlta_op3(std::span<const std::uint8_t> dlta,
                   std::uint8_t* fb,
                   unsigned int width,
                   unsigned int planes,
                   std::size_t  fb_size) {
        if (dlta.size() < static_cast<std::size_t>(planes) * 4u) {
            return make_unexpected("anim: DLTA op3 too small for plane-offset table");
        }
        const std::size_t cap     = dlta.size();
        const auto*       base    = dlta.data();
        const std::size_t planepitch = ((static_cast<std::size_t>(width) + 15u) / 16u) * 2u;
        const std::size_t pitch     = planepitch * planes;

        auto rb16 = [&](std::size_t& si) -> int {
            if (si + 1 >= cap) return -1;
            const auto v = static_cast<std::uint16_t>(
                (static_cast<unsigned>(base[si]) << 8u) | base[si + 1]);
            si += 2;
            return static_cast<int>(v);
        };

        for (unsigned int k = 0; k < planes; ++k) {
            const std::uint32_t off =
                (static_cast<std::uint32_t>(base[k * 4 + 0]) << 24u) |
                (static_cast<std::uint32_t>(base[k * 4 + 1]) << 16u) |
                (static_cast<std::uint32_t>(base[k * 4 + 2]) <<  8u) |
                 static_cast<std::uint32_t>(base[k * 4 + 3]);
            if (off == 0) continue;
            if (off >= cap) continue;
            std::size_t si = off;
            std::size_t pos = 0;
            while (true) {
                const int raw = rb16(si);
                if (raw < 0) return make_unexpected("anim: DLTA op3 truncated at offset");
                if (raw == 0xFFFF) break;
                const std::int16_t off16 = static_cast<std::int16_t>(raw);
                if (off16 >= 0) {
                    const int data = rb16(si);
                    if (data < 0) return make_unexpected("anim: DLTA op3 truncated at single-data");
                    pos += static_cast<std::size_t>(off16) * 2u;
                    const std::size_t real = (pos / planepitch) * pitch +
                                             (pos % planepitch) +
                                             static_cast<std::size_t>(k) * planepitch;
                    if (real + 1 < fb_size) {
                        fb[real]     = static_cast<std::uint8_t>(data >> 8);
                        fb[real + 1] = static_cast<std::uint8_t>(data & 0xFF);
                    }
                } else {
                    const int count = rb16(si);
                    if (count < 0) return make_unexpected("anim: DLTA op3 truncated at run-count");
                    pos += static_cast<std::size_t>(2 * (-(off16 + 2)));
                    for (int i = 0; i < count; ++i) {
                        const int data = rb16(si);
                        if (data < 0) return make_unexpected("anim: DLTA op3 truncated at run data");
                        pos += 2u;
                        const std::size_t real = (pos / planepitch) * pitch +
                                                 (pos % planepitch) +
                                                 static_cast<std::size_t>(k) * planepitch;
                        if (real + 1 < fb_size) {
                            fb[real]     = static_cast<std::uint8_t>(data >> 8);
                            fb[real + 1] = static_cast<std::uint8_t>(data & 0xFF);
                        }
                    }
                }
            }
        }
        return {};
    }

    namespace {
        // Helpers shared by ops 7/8 — read 1/2/4-byte BE values out of a span
        // with a movable cursor. Each returns -1 on truncation.
        struct cursor {
            const std::uint8_t* p;
            std::size_t cap;
            std::size_t pos;
        };
        inline int rd_u8(cursor& c) {
            if (c.pos >= c.cap) return -1;
            return c.p[c.pos++];
        }
        inline int rd_be16(cursor& c) {
            if (c.pos + 1 >= c.cap) return -1;
            const auto v = static_cast<std::uint16_t>(
                (static_cast<unsigned>(c.p[c.pos]) << 8u) | c.p[c.pos + 1]);
            c.pos += 2;
            return static_cast<int>(v);
        }
        inline std::int64_t rd_be32(cursor& c) {
            if (c.pos + 3 >= c.cap) return -1;
            const auto v = (static_cast<std::uint32_t>(c.p[c.pos]) << 24u) |
                           (static_cast<std::uint32_t>(c.p[c.pos + 1]) << 16u) |
                           (static_cast<std::uint32_t>(c.p[c.pos + 2]) <<  8u) |
                            static_cast<std::uint32_t>(c.p[c.pos + 3]);
            c.pos += 4;
            return v;
        }
        inline void put_be16(std::uint8_t* fb, std::size_t off, std::size_t cap,
                             std::uint16_t v) {
            if (off + 1 >= cap) return;
            fb[off]     = static_cast<std::uint8_t>(v >> 8);
            fb[off + 1] = static_cast<std::uint8_t>(v & 0xFF);
        }
        inline void put_be32(std::uint8_t* fb, std::size_t off, std::size_t cap,
                             std::uint32_t v) {
            if (off + 3 >= cap) return;
            fb[off]     = static_cast<std::uint8_t>(v >> 24);
            fb[off + 1] = static_cast<std::uint8_t>(v >> 16);
            fb[off + 2] = static_cast<std::uint8_t>(v >>  8);
            fb[off + 3] = static_cast<std::uint8_t>(v & 0xFF);
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
    apply_dlta_op7_short(std::span<const std::uint8_t> dlta,
                         std::uint8_t* fb,
                         unsigned int width,
                         unsigned int planes,
                         std::size_t  fb_size) {
        if (dlta.size() <= 64) {
            return make_unexpected("anim: DLTA op7-short too small");
        }
        const auto*       base = dlta.data();
        const std::size_t cap  = dlta.size();
        const unsigned    ncolumns = (width + 15u) >> 4u;
        const std::size_t dstpitch = static_cast<std::size_t>(ncolumns) * planes * 2u;

        cursor ptrs{base, cap, 0};
        cursor dptrs{base, cap, 32};

        for (unsigned int k = 0; k < planes; ++k) {
            const auto ofssrc  = rd_be32(ptrs);
            const auto ofsdata = rd_be32(dptrs);
            if (ofssrc < 0 || ofsdata < 0) {
                return make_unexpected("anim: DLTA op7-short header truncated");
            }
            if (ofssrc == 0) continue;
            if (static_cast<std::size_t>(ofssrc) >= cap ||
                static_cast<std::size_t>(ofsdata) >= cap) {
                continue; // matches ffmpeg: silently skip planes with bogus offsets
            }
            cursor gb{base, cap, static_cast<std::size_t>(ofssrc)};
            cursor dgb{base, cap, static_cast<std::size_t>(ofsdata)};

            for (unsigned int j = 0; j < ncolumns; ++j) {
                std::size_t ofsdst = (static_cast<std::size_t>(j) +
                                      static_cast<std::size_t>(k) * ncolumns) * 2u;
                const int op_count = rd_u8(gb);
                if (op_count < 0) return make_unexpected("anim: op7-short col header truncated");
                int i = op_count;
                while (i > 0) {
                    const int op = rd_u8(gb);
                    if (op < 0) return make_unexpected("anim: op7-short op truncated");
                    if (op == 0) {
                        const int count = rd_u8(gb);
                        const int x     = rd_be16(dgb);
                        if (count < 0 || x < 0) {
                            return make_unexpected("anim: op7-short SAME truncated");
                        }
                        for (int n = 0; n < count; ++n) {
                            put_be16(fb, ofsdst, fb_size, static_cast<std::uint16_t>(x));
                            ofsdst += dstpitch;
                        }
                    } else if (op < 0x80) {
                        ofsdst += static_cast<std::size_t>(op) * dstpitch;
                    } else {
                        const int count = op & 0x7F;
                        for (int n = 0; n < count; ++n) {
                            const int x = rd_be16(dgb);
                            if (x < 0) return make_unexpected("anim: op7-short UNIQUE truncated");
                            put_be16(fb, ofsdst, fb_size, static_cast<std::uint16_t>(x));
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
    apply_dlta_op7_long(std::span<const std::uint8_t> dlta,
                        std::uint8_t* fb,
                        unsigned int width,
                        unsigned int planes,
                        std::size_t  fb_size) {
        if (dlta.size() <= 64) {
            return make_unexpected("anim: DLTA op7-long too small");
        }
        const auto*       base = dlta.data();
        const std::size_t cap  = dlta.size();
        const unsigned    ncolumns = (width + 31u) >> 5u;
        const std::size_t dstpitch = (((static_cast<std::size_t>(width) + 15u) / 16u) * 2u) * planes;
        const bool        h        = ((((width + 15u) / 16u) * 2u) !=
                                      (((width + 31u) / 32u) * 4u));

        cursor ptrs{base, cap, 0};
        cursor dptrs{base, cap, 32};

        for (unsigned int k = 0; k < planes; ++k) {
            const auto ofssrc  = rd_be32(ptrs);
            const auto ofsdata = rd_be32(dptrs);
            if (ofssrc < 0 || ofsdata < 0) {
                return make_unexpected("anim: DLTA op7-long header truncated");
            }
            if (ofssrc == 0) continue;
            if (static_cast<std::size_t>(ofssrc) >= cap ||
                static_cast<std::size_t>(ofsdata) >= cap) {
                continue;
            }
            cursor gb{base, cap, static_cast<std::size_t>(ofssrc)};
            cursor dgb{base, cap, static_cast<std::size_t>(ofsdata)};

            for (unsigned int j = 0; j < ncolumns; ++j) {
                std::int64_t ofsdst = static_cast<std::int64_t>(
                    (static_cast<std::size_t>(j) +
                     static_cast<std::size_t>(k) * ncolumns) * 4u);
                ofsdst -= static_cast<std::int64_t>(h ? (2u * k) : 0u);
                const bool last_kludge = h && (j == (ncolumns - 1));
                const int op_count = rd_u8(gb);
                if (op_count < 0) return make_unexpected("anim: op7-long col header truncated");
                int i = op_count;
                while (i > 0) {
                    const int op = rd_u8(gb);
                    if (op < 0) return make_unexpected("anim: op7-long op truncated");
                    if (op == 0) {
                        const int count = rd_u8(gb);
                        if (count < 0) return make_unexpected("anim: op7-long SAME count truncated");
                        std::uint32_t x;
                        if (last_kludge) {
                            const int v = rd_be16(dgb);
                            if (v < 0) return make_unexpected("anim: op7-long SAME data truncated");
                            x = static_cast<std::uint32_t>(v);
                            (void)rd_be16(dgb); // skip 2 bytes
                        } else {
                            const auto v = rd_be32(dgb);
                            if (v < 0) return make_unexpected("anim: op7-long SAME data truncated");
                            x = static_cast<std::uint32_t>(v);
                        }
                        for (int n = 0; n < count; ++n) {
                            if (ofsdst < 0) { ofsdst += static_cast<std::int64_t>(dstpitch); continue; }
                            if (last_kludge) {
                                put_be16(fb, static_cast<std::size_t>(ofsdst), fb_size,
                                         static_cast<std::uint16_t>(x));
                            } else {
                                put_be32(fb, static_cast<std::size_t>(ofsdst), fb_size, x);
                            }
                            ofsdst += static_cast<std::int64_t>(dstpitch);
                        }
                    } else if (op < 0x80) {
                        ofsdst += static_cast<std::int64_t>(op) * static_cast<std::int64_t>(dstpitch);
                    } else {
                        const int count = op & 0x7F;
                        for (int n = 0; n < count; ++n) {
                            if (last_kludge) {
                                const int v = rd_be16(dgb);
                                if (v < 0) return make_unexpected("anim: op7-long UNIQUE data truncated");
                                if (ofsdst >= 0) {
                                    put_be16(fb, static_cast<std::size_t>(ofsdst), fb_size,
                                             static_cast<std::uint16_t>(v));
                                }
                                (void)rd_be16(dgb);
                            } else {
                                const auto v = rd_be32(dgb);
                                if (v < 0) return make_unexpected("anim: op7-long UNIQUE data truncated");
                                if (ofsdst >= 0) {
                                    put_be32(fb, static_cast<std::size_t>(ofsdst), fb_size,
                                             static_cast<std::uint32_t>(v));
                                }
                            }
                            ofsdst += static_cast<std::int64_t>(dstpitch);
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
    apply_dlta_op8_short(std::span<const std::uint8_t> dlta,
                         std::uint8_t* fb,
                         unsigned int width,
                         unsigned int planes,
                         std::size_t  fb_size) {
        if (dlta.size() < 64) {
            return make_unexpected("anim: DLTA op8-short too small");
        }
        const auto*       base = dlta.data();
        const std::size_t cap  = dlta.size();
        const unsigned    ncolumns = (width + 15u) >> 4u;
        const std::size_t dstpitch = static_cast<std::size_t>(ncolumns) * planes * 2u;

        cursor ptrs{base, cap, 0};

        for (unsigned int k = 0; k < planes; ++k) {
            const auto ofssrc = rd_be32(ptrs);
            if (ofssrc < 0) return make_unexpected("anim: DLTA op8-short header truncated");
            if (ofssrc == 0) continue;
            if (static_cast<std::size_t>(ofssrc) >= cap) {
                continue;
            }
            cursor gb{base, cap, static_cast<std::size_t>(ofssrc)};

            for (unsigned int j = 0; j < ncolumns; ++j) {
                std::size_t ofsdst = (static_cast<std::size_t>(j) +
                                      static_cast<std::size_t>(k) * ncolumns) * 2u;
                const int i_init = rd_be16(gb);
                if (i_init < 0) return make_unexpected("anim: op8-short col header truncated");
                int i = i_init;
                while (i > 0 && (gb.cap - gb.pos) > 4) {
                    const int op = rd_be16(gb);
                    if (op < 0) return make_unexpected("anim: op8-short op truncated");
                    if (op == 0) {
                        const int count = rd_be16(gb);
                        const int x     = rd_be16(gb);
                        if (count < 0 || x < 0) {
                            return make_unexpected("anim: op8-short SAME truncated");
                        }
                        for (int n = 0; n < count; ++n) {
                            put_be16(fb, ofsdst, fb_size, static_cast<std::uint16_t>(x));
                            ofsdst += dstpitch;
                        }
                    } else if (op < 0x8000) {
                        ofsdst += static_cast<std::size_t>(op) * dstpitch;
                    } else {
                        const int count = op & 0x7FFF;
                        for (int n = 0; n < count; ++n) {
                            const int x = rd_be16(gb);
                            if (x < 0) return make_unexpected("anim: op8-short UNIQUE truncated");
                            put_be16(fb, ofsdst, fb_size, static_cast<std::uint16_t>(x));
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
    apply_dlta_op8_long(std::span<const std::uint8_t> dlta,
                        std::uint8_t* fb,
                        unsigned int width,
                        unsigned int planes,
                        std::size_t  fb_size) {
        if (dlta.size() < 64) {
            return make_unexpected("anim: DLTA op8-long too small");
        }
        const auto*       base = dlta.data();
        const std::size_t cap  = dlta.size();
        const unsigned    ncolumns = (width + 31u) >> 5u;
        const std::size_t dstpitch = (((static_cast<std::size_t>(width) + 15u) / 16u) * 2u) * planes;
        const bool        h        = ((((width + 15u) / 16u) * 2u) !=
                                      (((width + 31u) / 32u) * 4u));

        cursor ptrs{base, cap, 0};

        for (unsigned int k = 0; k < planes; ++k) {
            const auto ofssrc = rd_be32(ptrs);
            if (ofssrc < 0) return make_unexpected("anim: DLTA op8-long header truncated");
            if (ofssrc == 0) continue;
            if (static_cast<std::size_t>(ofssrc) >= cap) {
                continue;
            }
            cursor gb{base, cap, static_cast<std::size_t>(ofssrc)};

            for (unsigned int j = 0; j < ncolumns; ++j) {
                std::int64_t ofsdst = static_cast<std::int64_t>(
                    (static_cast<std::size_t>(j) +
                     static_cast<std::size_t>(k) * ncolumns) * 4u);
                ofsdst -= static_cast<std::int64_t>(h ? (2u * k) : 0u);
                const bool last_kludge = h && (j == (ncolumns - 1));
                const std::uint32_t skip_mask = last_kludge ? 0x8000u : 0x80000000u;
                const std::uint32_t value_mask = skip_mask - 1u;

                const auto i_init = rd_be32(gb);
                if (i_init < 0) return make_unexpected("anim: op8-long col header truncated");
                std::int64_t i = i_init;
                while (i > 0 && (gb.cap - gb.pos) > 4) {
                    const auto op64 = rd_be32(gb);
                    if (op64 < 0) return make_unexpected("anim: op8-long op truncated");
                    const std::uint32_t op = static_cast<std::uint32_t>(op64);
                    if (op == 0) {
                        std::uint32_t count, x;
                        if (last_kludge) {
                            const int c = rd_be16(gb);
                            const int v = rd_be16(gb);
                            if (c < 0 || v < 0) return make_unexpected("anim: op8-long SAME truncated");
                            count = static_cast<std::uint32_t>(c);
                            x     = static_cast<std::uint32_t>(v);
                        } else {
                            const auto c = rd_be32(gb);
                            const auto v = rd_be32(gb);
                            if (c < 0 || v < 0) return make_unexpected("anim: op8-long SAME truncated");
                            count = static_cast<std::uint32_t>(c);
                            x     = static_cast<std::uint32_t>(v);
                        }
                        for (std::uint32_t n = 0; n < count; ++n) {
                            if (ofsdst >= 0) {
                                if (last_kludge) {
                                    put_be16(fb, static_cast<std::size_t>(ofsdst), fb_size,
                                             static_cast<std::uint16_t>(x));
                                } else {
                                    put_be32(fb, static_cast<std::size_t>(ofsdst), fb_size, x);
                                }
                            }
                            ofsdst += static_cast<std::int64_t>(dstpitch);
                        }
                    } else if (op < skip_mask) {
                        ofsdst += static_cast<std::int64_t>(op) * static_cast<std::int64_t>(dstpitch);
                    } else {
                        std::uint32_t count = op & value_mask;
                        for (std::uint32_t n = 0; n < count; ++n) {
                            if (last_kludge) {
                                const int v = rd_be16(gb);
                                if (v < 0) return make_unexpected("anim: op8-long UNIQUE truncated");
                                if (ofsdst >= 0) {
                                    put_be16(fb, static_cast<std::size_t>(ofsdst), fb_size,
                                             static_cast<std::uint16_t>(v));
                                }
                            } else {
                                const auto v = rd_be32(gb);
                                if (v < 0) return make_unexpected("anim: op8-long UNIQUE truncated");
                                if (ofsdst >= 0) {
                                    put_be32(fb, static_cast<std::size_t>(ofsdst), fb_size,
                                             static_cast<std::uint32_t>(v));
                                }
                            }
                            ofsdst += static_cast<std::int64_t>(dstpitch);
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
    apply_dlta_opj(std::span<const std::uint8_t> dlta,
                   std::uint8_t* fb,
                   unsigned int width,
                   unsigned int /*height*/,
                   unsigned int planes,
                   std::size_t  fb_size) {
        const auto*       base = dlta.data();
        const std::size_t cap  = dlta.size();

        const std::size_t planepitch_byte = (static_cast<std::size_t>(width) + 7u) / 8u;
        const std::size_t planepitch      = ((static_cast<std::size_t>(width) + 15u) / 16u) * 2u;
        const std::size_t pitch           = planepitch * planes;
        const std::size_t kludge_j        = (width < 320u) ? ((320u - width) / 8u / 2u) : 0u;

        cursor gb{base, cap, 0};

        auto translate_offset = [&](std::uint32_t off) -> std::int64_t {
            if (kludge_j) {
                return static_cast<std::int64_t>((off / (320u / 8u)) * pitch +
                                                 (off % (320u / 8u))) -
                       static_cast<std::int64_t>(kludge_j);
            }
            return static_cast<std::int64_t>((off / planepitch_byte) * pitch +
                                             (off % planepitch_byte));
        };

        while ((gb.cap - gb.pos) >= 2) {
            const int type = rd_be16(gb);
            if (type < 0) return make_unexpected("anim: opJ truncated at type");
            switch (type) {
                case 0:
                    return {};
                case 1: {
                    const int flag = rd_be16(gb);
                    const auto cols = rd_be16(gb);
                    const auto groups = rd_be16(gb);
                    if (flag < 0 || cols < 0 || groups < 0) {
                        return make_unexpected("anim: opJ type-1 header truncated");
                    }
                    for (int g = 0; g < groups; ++g) {
                        const auto raw_off = rd_be16(gb);
                        if (raw_off < 0) {
                            return make_unexpected("anim: opJ type-1 group offset truncated");
                        }
                        if (static_cast<std::size_t>(cols) * planes == 0 ||
                            (gb.cap - gb.pos) < static_cast<std::size_t>(cols) * planes) {
                            return make_unexpected("anim: opJ type-1 cols*planes invalid");
                        }
                        std::int64_t offset = translate_offset(static_cast<std::uint32_t>(raw_off));
                        for (int b = 0; b < cols; ++b) {
                            for (unsigned d = 0; d < planes; ++d) {
                                const int value = rd_u8(gb);
                                if (value < 0) {
                                    return make_unexpected("anim: opJ type-1 data truncated");
                                }
                                if (offset >= 0 &&
                                    static_cast<std::size_t>(offset) < fb_size) {
                                    if (flag) {
                                        fb[offset] = static_cast<std::uint8_t>(
                                            fb[offset] ^ static_cast<std::uint8_t>(value));
                                    } else {
                                        fb[offset] = static_cast<std::uint8_t>(value);
                                    }
                                }
                                offset += static_cast<std::int64_t>(planepitch);
                            }
                        }
                        if ((static_cast<std::size_t>(cols) * planes) & 1u) {
                            (void)rd_u8(gb); // alignment padding
                        }
                    }
                    break;
                }
                case 2: {
                    const int flag   = rd_be16(gb);
                    const int rows   = rd_be16(gb);
                    const int bytes  = rd_be16(gb);
                    const int groups = rd_be16(gb);
                    if (flag < 0 || rows < 0 || bytes < 0 || groups < 0) {
                        return make_unexpected("anim: opJ type-2 header truncated");
                    }
                    for (int g = 0; g < groups; ++g) {
                        const auto raw_off = rd_be16(gb);
                        if (raw_off < 0) {
                            return make_unexpected("anim: opJ type-2 group offset truncated");
                        }
                        std::int64_t offset = translate_offset(static_cast<std::uint32_t>(raw_off));
                        for (int r = 0; r < rows; ++r) {
                            for (unsigned d = 0; d < planes; ++d) {
                                std::int64_t noffset = offset +
                                    static_cast<std::int64_t>(r) * static_cast<std::int64_t>(pitch) +
                                    static_cast<std::int64_t>(d) * static_cast<std::int64_t>(planepitch);
                                if (bytes == 0 || (gb.cap - gb.pos) < static_cast<std::size_t>(bytes)) {
                                    return make_unexpected("anim: opJ type-2 bytes invalid");
                                }
                                for (int b = 0; b < bytes; ++b) {
                                    const int value = rd_u8(gb);
                                    if (value < 0) {
                                        return make_unexpected("anim: opJ type-2 data truncated");
                                    }
                                    if (noffset >= 0 &&
                                        static_cast<std::size_t>(noffset) < fb_size) {
                                        if (flag) {
                                            fb[noffset] = static_cast<std::uint8_t>(
                                                fb[noffset] ^ static_cast<std::uint8_t>(value));
                                        } else {
                                            fb[noffset] = static_cast<std::uint8_t>(value);
                                        }
                                    }
                                    ++noffset;
                                }
                            }
                        }
                        if ((static_cast<std::size_t>(rows) * static_cast<std::size_t>(bytes) * planes) & 1u) {
                            (void)rd_u8(gb);
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
    apply_dlta_op_l(std::span<const std::uint8_t> dlta,
                    std::uint8_t* fb,
                    unsigned int width,
                    unsigned int planes,
                    bool         is_short,
                    std::size_t  fb_size) {
        if (dlta.size() <= 64) {
            return make_unexpected("anim: DLTA opL too small");
        }
        const auto*       base = dlta.data();
        const std::size_t cap  = dlta.size();

        const std::size_t planepitch_byte =
            (static_cast<std::size_t>(width) + 7u) / 8u;
        const std::size_t planepitch =
            ((static_cast<std::size_t>(width) + 15u) / 16u) * 2u;
        const std::size_t pitch    = planepitch * planes;
        const std::size_t dstpitch = is_short ? (planepitch_byte * planes) : 2u;

        if (planepitch_byte == 0) {
            return make_unexpected("anim: opL zero plane pitch");
        }

        // dptrs reads data offsets (table at 0); optrs reads opcode offsets
        // (table at 32). Naming mirrors ffmpeg's poff0/poff1.
        cursor dptrs{base, cap, 0};
        cursor optrs{base, cap, 32};

        for (unsigned int k = 0; k < planes; ++k) {
            const auto poff_data = rd_be32(dptrs);
            const auto poff_ops  = rd_be32(optrs);
            if (poff_data < 0 || poff_ops < 0) {
                return make_unexpected("anim: opL plane offset table truncated");
            }
            if (poff_data == 0) continue;

            // Word offsets → byte offsets (×2). Bail on out-of-range to mirror
            // ffmpeg behaviour (it returns silently rather than failing the
            // whole frame; stay strict here so problems are visible).
            const std::size_t data_pos = static_cast<std::size_t>(poff_data) * 2u;
            const std::size_t ops_pos  = static_cast<std::size_t>(poff_ops)  * 2u;
            if (data_pos >= cap || ops_pos >= cap) {
                return make_unexpected("anim: opL plane stream offset past end");
            }

            cursor dgb{base, cap, data_pos};
            cursor ogb{base, cap, ops_pos};

            // Walk records until the terminator (peek == 0xFFFF) or stream end.
            for (;;) {
                if (ogb.pos + 4u > ogb.cap) break;
                const std::uint16_t peek = static_cast<std::uint16_t>(
                    (static_cast<unsigned>(ogb.p[ogb.pos]) << 8u) |
                    ogb.p[ogb.pos + 1]);
                if (peek == 0xFFFFu) break;

                const int raw_off = rd_be16(ogb);
                const int raw_cnt = rd_be16(ogb);
                if (raw_off < 0 || raw_cnt < 0) {
                    return make_unexpected("anim: opL record truncated");
                }
                const std::int16_t cnt =
                    static_cast<std::int16_t>(static_cast<std::uint16_t>(raw_cnt));

                const std::size_t off_bytes_in_plane =
                    static_cast<std::size_t>(raw_off) * 2u;
                const std::size_t row    = off_bytes_in_plane / planepitch_byte;
                const std::size_t colbyt = off_bytes_in_plane % planepitch_byte;
                std::size_t addr =
                    row * pitch + colbyt + static_cast<std::size_t>(k) * planepitch;

                if (cnt < 0) {
                    const int data = rd_be16(dgb);
                    if (data < 0) return make_unexpected("anim: opL run datum truncated");
                    const int reps = -static_cast<int>(cnt);
                    for (int i = 0; i < reps; ++i) {
                        put_be16(fb, addr, fb_size, static_cast<std::uint16_t>(data));
                        addr += dstpitch;
                    }
                } else {
                    for (int i = 0; i < cnt; ++i) {
                        const int data = rd_be16(dgb);
                        if (data < 0) {
                            return make_unexpected("anim: opL literal data truncated");
                        }
                        put_be16(fb, addr, fb_size, static_cast<std::uint16_t>(data));
                        addr += dstpitch;
                    }
                }
            }
        }
        return {};
    }
} // namespace anim
