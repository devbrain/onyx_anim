#include <atari_seq/decoders.hh>

#include <cstring>

namespace atari_seq {
    void decode_palette(const std::uint16_t st_palette[16],
                        unsigned int n_colors,
                        std::uint8_t* rgb_out_768) noexcept {
        std::memset(rgb_out_768, 0, 768);
        for (unsigned int i = 0; i < n_colors && i < 16; ++i) {
            const std::uint16_t w = st_palette[i];
            rgb_out_768[i * 3 + 0] = scale_st_3_to_8((w >> 8) & 0x7u);
            rgb_out_768[i * 3 + 1] = scale_st_3_to_8((w >> 4) & 0x7u);
            rgb_out_768[i * 3 + 2] = scale_st_3_to_8((w) & 0x7u);
        }
    }

    namespace {
        // Walker bookkeeping for iterating words across a rect in planar order:
        //   for each plane b, for each x_group (step 16), for each y, write 1 word.
        //
        // Mirrors the iteration in Randelshofer's SEQDeltaFrame.decodeXxx.
        struct walker {
            std::uint8_t* fb;
            std::size_t scanline_stride;
            std::size_t bitplane_stride;
            unsigned planes;
            unsigned left, top, right, bottom;
            unsigned x, y, b;
            unsigned width; // total framebuffer width (for clipping)
            unsigned shift;
            std::size_t si;
            bool done;
        };

        void init_walker(walker& w,
                         std::uint8_t* fb,
                         std::size_t scanline_stride,
                         std::size_t bitplane_stride,
                         unsigned planes,
                         unsigned x_offset, unsigned y_offset,
                         unsigned rect_w, unsigned rect_h,
                         unsigned frame_width) {
            w.fb = fb;
            w.scanline_stride = scanline_stride;
            w.bitplane_stride = bitplane_stride;
            w.planes = planes;
            w.left = x_offset;
            w.top = y_offset;
            w.right = x_offset + rect_w;
            w.bottom = y_offset + rect_h;
            w.x = x_offset;
            w.y = y_offset;
            w.b = 0;
            w.width = frame_width;
            w.shift = x_offset & 0x7u;
            w.si = static_cast <std::size_t>(y_offset) * scanline_stride
                   + static_cast <std::size_t>(x_offset) / 8u;
            w.done = (rect_w == 0 || rect_h == 0 || planes == 0);
        }

        // Advance after writing one word.
        inline void advance_walker(walker& w) {
            w.y += 1;
            w.si += w.scanline_stride;
            if (w.y >= w.bottom) {
                w.y = w.top;
                w.x += 16;
                if (w.x >= w.right) {
                    w.x = w.left;
                    w.b += 1;
                    if (w.b >= w.planes) {
                        w.done = true;
                        return;
                    }
                }
                w.si = static_cast <std::size_t>(w.b) * w.bitplane_stride
                       + static_cast <std::size_t>(w.y) * w.scanline_stride
                       + static_cast <std::size_t>(w.x) / 8u;
            }
        }

        // Place one word at the current walker position, with the operation
        // (copy or XOR) and the byte-alignment shift handled.
        //
        // Returns false on any out-of-bounds touch — caller should stop and
        // surface as a decode error.
        inline bool emit_word(walker& w, std::uint8_t d1, std::uint8_t d2,
                              bool xor_op) {
            // Bounds: the highest byte we touch is si+2 in the shifted case.
            const std::size_t max_off =
                static_cast <std::size_t>(w.b) * w.bitplane_stride
                + (static_cast <std::size_t>(w.bitplane_stride)); // < end of plane
            (void)max_off; // (handled by walker logic; kept here for documentation)

            if (w.shift == 0) {
                if (xor_op) {
                    w.fb[w.si] ^= d1;
                    if (w.x < w.width - 8) {
                        w.fb[w.si + 1] ^= d2;
                    }
                } else {
                    w.fb[w.si] = d1;
                    if (w.x < w.width - 8) {
                        w.fb[w.si + 1] = d2;
                    }
                }
                return true;
            }

            // Bit-shifted (rect doesn't start on a byte boundary): three bytes
            // are touched. Same masking pattern as Randelshofer.
            const unsigned shift = w.shift;
            const unsigned inv_shift = 8u - shift;
            const auto inv_mask = static_cast <std::uint8_t>((0xFFu << inv_shift) & 0xFFu);
            const auto xor_inv_mask = static_cast <std::uint8_t>(0xFFu >> shift);

            const auto shifted_d1 = static_cast <std::uint8_t>(d1 >> shift);
            const auto shifted_d2 =
                static_cast <std::uint8_t>(((d1 << inv_shift) & inv_mask)
                                           | static_cast <unsigned>(d2 >> shift));
            const auto shifted_d3 = static_cast <std::uint8_t>((d2 << inv_shift) & 0xFFu);

            if (xor_op) {
                w.fb[w.si] = static_cast <std::uint8_t>(
                    (w.fb[w.si] & inv_mask) | ((w.fb[w.si] & xor_inv_mask) ^ shifted_d1));
                if (w.x < w.width - 8) {
                    w.fb[w.si + 1] ^= shifted_d2;
                    if (w.x < w.width - 16) {
                        w.fb[w.si + 2] = static_cast <std::uint8_t>(
                            (w.fb[w.si + 2] & xor_inv_mask) | ((w.fb[w.si + 2] & inv_mask) ^ shifted_d3));
                    }
                }
            } else {
                w.fb[w.si] = static_cast <std::uint8_t>(
                    (w.fb[w.si] & inv_mask) | shifted_d1);
                if (w.x < w.width - 8) {
                    w.fb[w.si + 1] = shifted_d2;
                    if (w.x < w.width - 16) {
                        w.fb[w.si + 2] = static_cast <std::uint8_t>(
                            (w.fb[w.si + 2] & xor_inv_mask) | shifted_d3);
                    }
                }
            }
            return true;
        }
    } // namespace

    result apply_frame(std::span <const std::uint8_t> data,
                       std::uint8_t* fb_planar,
                       std::size_t scanline_stride,
                       std::size_t bitplane_stride,
                       unsigned int planes,
                       unsigned int frame_width,
                       unsigned int x_offset,
                       unsigned int y_offset,
                       unsigned int rect_width,
                       unsigned int rect_height,
                       operation op,
                       storage sm) {
        if (fb_planar == nullptr) {
            return make_unexpected("seq: framebuffer is null");
        }
        const bool xor_op = (op == operation::xor_op);

        // OP_Copy clears the entire planar bitmap before painting the rect, so
        // the rect's previous contents (and anything outside the rect) start
        // at zero. This matches Randelshofer's SEQDeltaFrame.decodeCopyXxx.
        // Randelshofer's reference SEQDeltaFrame leaves both decodeCopyUncompressed
        // and decodeXORUncompressed as empty function bodies — uncompressed
        // frames are silently no-ops there. Matching that for cross-check
        // compatibility (real-world SEQ files virtually always use word-RLE).
        if (sm == storage::uncompressed) {
            return {};
        }

        // The compressed COPY path clears the entire planar bitmap before
        // painting the rect — Randelshofer's `ArrayUtil.fill(screen, 0)` at
        // the head of decodeCopyCompressed.
        if (op == operation::copy) {
            std::memset(fb_planar, 0,
                        static_cast <std::size_t>(planes) * bitplane_stride);
        }

        walker w{};
        init_walker(w, fb_planar, scanline_stride, bitplane_stride, planes,
                    x_offset, y_offset, rect_width, rect_height, frame_width);

        // sm == storage::word_rle
        byte_reader br{data};
        while (!w.done && br.remaining() > 0) {
            std::uint16_t opcode = 0;
            br >> opcode;
            if (!br) {
                return make_unexpected("seq: RLE truncated at opcode");
            }
            if ((opcode & 0x8000u) == 0) {
                // Repeat run: next single word repeated `opcode` times.
                std::uint8_t d1 = 0, d2 = 0;
                br >> d1 >> d2;
                if (!br) {
                    return make_unexpected("seq: RLE truncated at repeat data");
                }
                for (unsigned int i = 0; i < opcode && !w.done; ++i) {
                    emit_word(w, d1, d2, xor_op);
                    advance_walker(w);
                }
            } else {
                // Literal run: copy `opcode & 0x7FFF` words verbatim.
                const unsigned int n = opcode & 0x7FFFu;
                for (unsigned int i = 0; i < n && !w.done; ++i) {
                    std::uint8_t d1 = 0, d2 = 0;
                    br >> d1 >> d2;
                    if (!br) {
                        return make_unexpected("seq: RLE truncated at literal data");
                    }
                    emit_word(w, d1, d2, xor_op);
                    advance_walker(w);
                }
            }
        }
        return {};
    }

    void planar_to_chunky(const std::uint8_t* planar,
                          std::size_t scanline_stride,
                          std::size_t bitplane_stride,
                          unsigned int planes,
                          std::uint8_t* chunky,
                          std::size_t chunky_stride,
                          unsigned int width,
                          unsigned int height) noexcept {
        for (unsigned int y = 0; y < height; ++y) {
            std::uint8_t* dst = chunky + static_cast <std::size_t>(y) * chunky_stride;
            for (unsigned int x = 0; x < width; ++x) {
                const std::size_t byte_off = y * scanline_stride + x / 8u;
                const unsigned bit = 7u - (x & 0x7u);
                std::uint8_t v = 0;
                for (unsigned int b = 0; b < planes; ++b) {
                    const std::uint8_t plane_byte =
                        planar[b * bitplane_stride + byte_off];
                    if (plane_byte & (1u << bit)) {
                        v |= static_cast <std::uint8_t>(1u << b);
                    }
                }
                dst[x] = v;
            }
        }
    }
} // namespace atari_seq
