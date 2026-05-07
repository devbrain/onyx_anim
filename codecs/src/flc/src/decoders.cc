#include <flc/decoders.hh>

#include <cstring>

namespace flc {
    // All sub-chunk decoders below are stubs; real implementations come next.
    // The signatures and contracts are fixed so the codec shell and tests can
    // wire against them today.

    // BYTE_RUN (FLI_BRUN, sub-chunk 15): full-frame, byte-oriented RLE.
    //
    // Per Animator Pro source (UNBRUN.C) and the FLIC spec:
    //   - Each line is encoded independently, top to bottom.
    //   - The first byte of each line is the obsolete packet count and MUST be
    //     ignored (FLI files capped at 255; FLC files routinely exceed it).
    //   - Decoding continues per-line until `width` pixels are emitted.
    //   - Each packet starts with a signed count byte:
    //       count >= 0 : replicate run — single data byte repeated `count` times.
    //       count <  0 : literal run — abs(count) data bytes copied verbatim.
    //   - Sign convention is the *opposite* of DELTA_FLI/LC.
    //
    // Robustness: bounds-check both the source span and the destination row.
    result decode_brun(std::span <const std::uint8_t> data,
                       std::uint8_t* fb, std::size_t pitch,
                       unsigned int width, unsigned int height) {
        if (fb == nullptr) {
            return make_unexpected("flc: BRUN destination is null");
        }
        bytes::byte_reader br{data};
        for (unsigned int y = 0; y < height; ++y) {
            // Skip the obsolete per-line packet count byte.
            std::uint8_t obsolete = 0;
            br >> obsolete;
            if (!br) return make_unexpected("flc: BRUN truncated at line opcount");

            std::uint8_t* row = fb + static_cast <std::size_t>(y) * pitch;
            unsigned int x = 0;
            while (x < width) {
                std::int8_t count = 0;
                br >> count;
                if (!br) return make_unexpected("flc: BRUN truncated at packet count");

                if (count >= 0) {
                    // Replicate run: single byte repeated `count` times.
                    std::uint8_t v = 0;
                    br >> v;
                    if (!br) return make_unexpected("flc: BRUN truncated at replicate byte");
                    const auto n = static_cast <unsigned int>(count);
                    if (n > width - x) {
                        return make_unexpected("flc: BRUN replicate run overflows row");
                    }
                    std::memset(row + x, v, n);
                    x += n;
                } else {
                    // Literal run: abs(count) bytes copied verbatim.
                    const auto n = static_cast <unsigned int>(-static_cast <int>(count));
                    if (n > width - x) {
                        return make_unexpected("flc: BRUN literal run overflows row");
                    }
                    if (!br.has(n)) {
                        return make_unexpected("flc: BRUN literal run truncated");
                    }
                    std::memcpy(row + x, br.peek(), n);
                    br >> bytes::skip(n);
                    x += n;
                }
            }
        }
        return {};
    }

    // DELTA_FLI / FLI_LC (sub-chunk 12): byte-oriented delta against the
    // previous frame. Per spec:
    //   u16 line_skip   — number of unchanged lines from the top
    //   u16 line_count  — number of data-bearing lines that follow
    //   per line:
    //     u8  packet_count
    //     per packet:
    //       u8  column_skip   — pixels to leave untouched from current x
    //       i8  rle_count
    //         > 0: literal run — count bytes copied
    //         < 0: replicate run — abs(count) bytes from one data byte
    //         = 0: continuation — split skip across two packets when skip > 255
    //
    // Sign convention is INVERTED relative to BRUN: positive=literal, negative=replicate.
    result decode_lc(std::span <const std::uint8_t> data,
                     std::uint8_t* fb, std::size_t pitch,
                     unsigned int width, unsigned int height) {
        if (fb == nullptr) {
            return make_unexpected("flc: LC destination is null");
        }
        const std::uint8_t* p = data.data();
        const std::uint8_t* const end = data.data() + data.size();
        const auto has = [&p, end](std::size_t n) noexcept {
            return static_cast<std::size_t>(end - p) >= n;
        };

        if (!has(4)) return make_unexpected("flc: LC truncated at chunk header");
        const std::uint16_t line_skip = bytes::read_u16le(p);
        const std::uint16_t line_count = bytes::read_u16le(p + 2);
        p += 4;

        if (static_cast <std::size_t>(line_skip) +
            static_cast <std::size_t>(line_count) > height) {
            return make_unexpected("flc: LC line range exceeds frame height");
        }

        for (unsigned int li = 0; li < line_count; ++li) {
            const unsigned int y = static_cast <unsigned int>(line_skip) + li;
            std::uint8_t* dst = fb + static_cast <std::size_t>(y) * pitch;
            unsigned int remaining = width;

            if (!has(1)) return make_unexpected("flc: LC truncated at packet count");
            const std::uint8_t packet_count = *p++;

            for (unsigned int packet = 0; packet < packet_count; ++packet) {
                if (!has(2)) return make_unexpected("flc: LC truncated at packet header");
                const std::uint8_t col_skip = p[0];
                const std::int8_t rle_count = static_cast<std::int8_t>(p[1]);
                p += 2;

                if (col_skip > remaining) {
                    return make_unexpected("flc: LC column skip overflows row");
                }
                dst += col_skip;
                remaining -= col_skip;

                if (rle_count == 0) {
                    // Skip-continuation packet: no data, just the cumulative skip.
                    continue;
                }
                if (rle_count > 0) {
                    // Literal: rle_count bytes copied verbatim.
                    const auto n = static_cast <unsigned int>(rle_count);
                    if (!has(n)) {
                        return make_unexpected("flc: LC literal run truncated");
                    }
                    if (n > remaining) {
                        return make_unexpected("flc: LC literal run overflows row");
                    }
                    if (n == 1u) {
                        *dst = *p;
                    } else {
                        std::memcpy(dst, p, n);
                    }
                    p += n;
                    dst += n;
                    remaining -= n;
                } else {
                    // Replicate: abs(count) repetitions of the next byte.
                    const auto n =
                        static_cast <unsigned int>(-static_cast <int>(rle_count));
                    if (!has(1)) return make_unexpected("flc: LC truncated at replicate byte");
                    const std::uint8_t v = *p++;
                    if (n > remaining) {
                        return make_unexpected("flc: LC replicate run overflows row");
                    }
                    std::memset(dst, v, n);
                    dst += n;
                    remaining -= n;
                }
            }
        }
        return {};
    }

    // DELTA_FLC / FLI_SS2 (sub-chunk 7): word-oriented delta. Per spec:
    //   u16 line_count   — number of data-bearing lines
    //   per line: one or more 16-bit opcodes, then packets
    //     opcode top-2-bits:
    //       00 — opcode is the packet count for this line (terminates opcodes)
    //       01 — undefined
    //       10 — opcode's low byte is the value of the line's last pixel
    //            (used for odd-width frames); opcode list continues
    //       11 — abs(opcode as int16) is line skip count; opcode list continues
    //   per packet (when packet count > 0):
    //     u8  column_skip
    //     i8  rle_count   — same sign convention as DELTA_FLI:
    //                       > 0 literal of `count` words
    //                       < 0 replicate of |count| copies of one data word
    //                       = 0 skip-continuation
    //
    // "Words" are pairs of pixels (2 bytes for 8bpp).
    result decode_ss2(std::span <const std::uint8_t> data,
                      std::uint8_t* fb, std::size_t pitch,
                      unsigned int width, unsigned int height) {
        if (fb == nullptr) {
            return make_unexpected("flc: SS2 destination is null");
        }
        const std::uint8_t* p = data.data();
        const std::uint8_t* const end = data.data() + data.size();
        const auto has = [&p, end](std::size_t n) noexcept {
            return static_cast<std::size_t>(end - p) >= n;
        };

        if (!has(2)) return make_unexpected("flc: SS2 truncated at line_count");
        const std::uint16_t line_count = bytes::read_u16le(p);
        p += 2;

        unsigned int y = 0;
        for (unsigned int li = 0; li < line_count; ++li) {
            // ---- Read opcodes until we hit a packet count.
            if (!has(2)) return make_unexpected("flc: SS2 truncated at line opcode");
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

                    switch (tag) {
                        case 0b11u: {
                            // line skip
                            const auto signed_op = static_cast <std::int16_t>(opcode);
                            const auto skip = static_cast <unsigned int>(-signed_op);
                            y += skip;
                            if (y > height) {
                                return make_unexpected("flc: SS2 line skip overflows height");
                            }
                            break;
                        }
                        case 0b10u: {
                            // last-byte store
                            if (y >= height) {
                                return make_unexpected("flc: SS2 last-byte at oob row");
                            }
                            if (width == 0) {
                                return make_unexpected("flc: SS2 last-byte on zero-width frame");
                            }
                            const auto v = static_cast <std::uint8_t>(opcode & 0xFFu);
                            fb[y * pitch + (width - 1)] = v;
                            break;
                        }
                        case 0b01u:
                        default:
                            return make_unexpected("flc: SS2 undefined opcode tag");
                    }

                    if (!has(2)) return make_unexpected("flc: SS2 truncated at line opcode");
                    opcode = bytes::read_u16le(p);
                    p += 2;
                }
            }

            if (y >= height) {
                return make_unexpected("flc: SS2 packets on out-of-range row");
            }
            std::uint8_t* row = fb + static_cast <std::size_t>(y) * pitch;
            unsigned int x = 0;

            for (unsigned int packet = 0; packet < packet_count; ++packet) {
                if (!has(2)) return make_unexpected("flc: SS2 truncated at packet header");
                const std::uint8_t col_skip = p[0];
                const std::int8_t rle_count = static_cast<std::int8_t>(p[1]);
                p += 2;

                x += col_skip;
                if (x > width) {
                    return make_unexpected("flc: SS2 column skip overflows row");
                }

                if (rle_count == 0) {
                    continue; // skip-continuation
                }
                if (rle_count > 0) {
                    // Literal: rle_count words copied verbatim.
                    const auto n_words = static_cast <unsigned int>(rle_count);
                    const unsigned int n_bytes = n_words * 2u;
                    if (!has(n_bytes)) {
                        return make_unexpected("flc: SS2 literal run truncated");
                    }
                    if (n_bytes > width - x) {
                        return make_unexpected("flc: SS2 literal overflows row");
                    }
                    if (n_words == 1u) {
                        row[x] = p[0];
                        row[x + 1u] = p[1];
                    } else {
                        std::memcpy(row + x, p, n_bytes);
                    }
                    p += n_bytes;
                    x += n_bytes;
                } else {
                    // Replicate: abs(count) copies of one word.
                    const auto n_words =
                        static_cast <unsigned int>(-static_cast <int>(rle_count));
                    const unsigned int n_bytes = n_words * 2u;
                    if (!has(2)) return make_unexpected("flc: SS2 truncated at replicate word");
                    const std::uint8_t lo = p[0];
                    const std::uint8_t hi = p[1];
                    p += 2;
                    if (n_bytes > width - x) {
                        return make_unexpected("flc: SS2 replicate overflows row");
                    }
                    if (lo == hi) {
                        std::memset(row + x, lo, n_bytes);
                    } else {
                        for (unsigned int i = 0; i < n_words; ++i) {
                            row[x + i * 2u] = lo;
                            row[x + i * 2u + 1] = hi;
                        }
                    }
                    x += n_bytes;
                }
            }

            ++y;
        }
        return {};
    }

    result decode_copy(std::span <const std::uint8_t> data,
                       std::uint8_t* fb, std::size_t pitch,
                       unsigned int width, unsigned int height) {
        const std::size_t expected_bytes =
            static_cast <std::size_t>(width) * static_cast <std::size_t>(height);
        if (data.size() < expected_bytes) {
            return make_unexpected("flc: COPY chunk truncated");
        }
        for (unsigned int y = 0; y < height; ++y) {
            std::memcpy(fb + y * pitch,
                        data.data() + static_cast <std::size_t>(y) * width,
                        width);
        }
        return {};
    }

    void decode_black(std::uint8_t* fb, std::size_t pitch,
                      unsigned int width, unsigned int height) noexcept {
        for (unsigned int y = 0; y < height; ++y) {
            std::memset(fb + y * pitch, 0, width);
        }
    }

    namespace {
        // Replicate-shift conversion 0..63 → 0..255 (so 63 maps to 255, not 252).
        constexpr std::uint8_t scale_6_to_8(std::uint8_t v) noexcept {
            return static_cast <std::uint8_t>(
                ((static_cast <unsigned>(v) << 2) | (static_cast <unsigned>(v) >> 4)) & 0xFFu);
        }

        // Common decoder for COLOR_256 (sub-chunk 4) and COLOR_64 (sub-chunk 11).
        // Both have the same packet structure; only the RGB component range differs.
        //
        //   word: number of packets
        //   per packet:
        //     byte: skip count (palette indices to leave untouched)
        //     byte: copy count (RGB triplets to follow; 0 means 256)
        //     N RGB triplets
        result decode_color_chunk(std::span <const std::uint8_t> data,
                                  std::uint8_t* palette,
                                  bool from_6bit) {
            if (palette == nullptr) {
                return make_unexpected("flc: COLOR destination is null");
            }
            bytes::byte_reader br{data};
            std::uint16_t packet_count = 0;
            br >> packet_count;
            if (!br) return make_unexpected("flc: COLOR truncated at packet count");

            unsigned int idx = 0;
            for (unsigned int p = 0; p < packet_count; ++p) {
                std::uint8_t skip_count = 0;
                std::uint8_t copy_count = 0;
                br >> skip_count >> copy_count;
                if (!br) return make_unexpected("flc: COLOR truncated at packet header");

                idx += skip_count;
                const unsigned int n = (copy_count == 0)
                                           ? 256u
                                           : static_cast <unsigned int>(copy_count);

                if (idx > 256u || n > 256u - idx) {
                    return make_unexpected("flc: COLOR packet overflows 256-entry palette");
                }
                if (!br.has(static_cast <std::size_t>(n) * 3u)) {
                    return make_unexpected("flc: COLOR packet truncated");
                }

                for (unsigned int i = 0; i < n; ++i, ++idx) {
                    std::uint8_t r = 0, g = 0, b = 0;
                    br >> r >> g >> b;
                    if (from_6bit) {
                        r = scale_6_to_8(r);
                        g = scale_6_to_8(g);
                        b = scale_6_to_8(b);
                    }
                    palette[idx * 3 + 0] = r;
                    palette[idx * 3 + 1] = g;
                    palette[idx * 3 + 2] = b;
                }
            }
            return {};
        }
    } // namespace

    result decode_color_64(std::span <const std::uint8_t> data,
                           std::uint8_t* palette) {
        return decode_color_chunk(data, palette, /*from_6bit=*/true);
    }

    result decode_color_256(std::span <const std::uint8_t> data,
                            std::uint8_t* palette) {
        return decode_color_chunk(data, palette, /*from_6bit=*/false);
    }
} // namespace flc
