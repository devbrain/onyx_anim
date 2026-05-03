#include <bink/decoders.hh>
#include <bink/data.hh>
#include <bink/idct.hh>

#include <algorithm>
#include <cstring>

namespace bink {
    namespace {
        // Block types — same numeric IDs as ffmpeg's enum BlockTypes.
        enum block_type : int {
            kSkip    = 0,
            kScaled  = 1,
            kMotion  = 2,
            kRun     = 3,
            kResidue = 4,
            kIntra   = 5,
            kFill    = 6,
            kInter   = 7,
            kPattern = 8,
            kRaw     = 9,
        };

        // ---- read_dct_coeffs (port of ffmpeg's same-named function) ----
        //
        // Decodes 8x8 DCT coefficients via the band-walking scheme: starts
        // with three "band heads" at offsets 4, 24, 44 (in scan-order),
        // plus three single-coefficient probes at indices 1, 2, 3. As bits
        // are consumed each entry is processed in one of four modes that
        // expand bands progressively, eventually emitting concrete
        // coefficient values + sign bits.
        //
        // Returns either the quantizer index (0..15) on success or -1 on
        // failure. `q == -1` means "read quant_idx from bit stream"; any
        // non-negative value means "use this quant_idx, don't read".
        int read_dct_coeffs(bit_reader& br, std::int32_t block[64],
                            const std::uint8_t* scan,
                            int& coef_count_out, int coef_idx[64], int q) {
            int coef_list[128];
            int mode_list[128];
            int list_start = 64, list_end = 64;
            int coef_count = 0;

            if (br.bits_left() < 4) return -1;

            coef_list[list_end] = 4;  mode_list[list_end++] = 0;
            coef_list[list_end] = 24; mode_list[list_end++] = 0;
            coef_list[list_end] = 44; mode_list[list_end++] = 0;
            coef_list[list_end] = 1;  mode_list[list_end++] = 3;
            coef_list[list_end] = 2;  mode_list[list_end++] = 3;
            coef_list[list_end] = 3;  mode_list[list_end++] = 3;

            for (int bits = static_cast <int>(br.get_bits(4)) - 1; bits >= 0; --bits) {
                int list_pos = list_start;
                while (list_pos < list_end) {
                    if ((mode_list[list_pos] | coef_list[list_pos]) == 0 ||
                        br.get_bit() == 0u) {
                        ++list_pos;
                        continue;
                    }
                    int ccoef = coef_list[list_pos];
                    int mode  = mode_list[list_pos];
                    switch (mode) {
                        case 0:
                            coef_list[list_pos] = ccoef + 4;
                            mode_list[list_pos] = 1;
                            [[fallthrough]];
                        case 2:
                            if (mode == 2) {
                                coef_list[list_pos] = 0;
                                mode_list[list_pos++] = 0;
                            }
                            for (int i = 0; i < 4; ++i, ++ccoef) {
                                if (br.get_bit() != 0u) {
                                    coef_list[--list_start] = ccoef;
                                    mode_list[  list_start] = 3;
                                } else {
                                    int t;
                                    if (bits == 0) {
                                        t = 1 - (static_cast <int>(br.get_bit()) << 1);
                                    } else {
                                        t = static_cast <int>(br.get_bits(static_cast <unsigned int>(bits))) |
                                    (1 << bits);
                                        const int sign = br.get_bit() ? -1 : 0;
                                        t = (t ^ sign) - sign;
                                    }
                                    block[scan[ccoef]] = t;
                                    coef_idx[coef_count++] = ccoef;
                                }
                            }
                            break;
                        case 1:
                            mode_list[list_pos] = 2;
                            for (int i = 0; i < 3; ++i) {
                                ccoef += 4;
                                coef_list[list_end]   = ccoef;
                                mode_list[list_end++] = 2;
                            }
                            break;
                        case 3: {
                            int t;
                            if (bits == 0) {
                                t = 1 - (static_cast <int>(br.get_bit()) << 1);
                            } else {
                                t = static_cast <int>(br.get_bits(static_cast <unsigned int>(bits))) |
                                    (1 << bits);
                                const int sign = br.get_bit() ? -1 : 0;
                                t = (t ^ sign) - sign;
                            }
                            block[scan[ccoef]] = t;
                            coef_idx[coef_count++] = ccoef;
                            coef_list[list_pos] = 0;
                            mode_list[list_pos++] = 0;
                            break;
                        }
                    }
                }
            }

            int quant_idx;
            if (q == -1) {
                quant_idx = static_cast <int>(br.get_bits(4));
            } else {
                quant_idx = q;
                if (quant_idx > 15) return -1;
            }
            coef_count_out = coef_count;
            return quant_idx;
        }

        void unquantize_dct_coeffs(std::int32_t block[64],
                                   const std::int32_t quant[64],
                                   int coef_count, const int coef_idx[64],
                                   const std::uint8_t* scan) noexcept {
            block[0] = (block[0] * quant[0]) >> 11;
            for (int i = 0; i < coef_count; ++i) {
                const int idx = coef_idx[i];
                block[scan[idx]] = (block[scan[idx]] * quant[idx]) >> 11;
            }
        }

        // ---- read_residue (port of ffmpeg's same-named function) ----
        int read_residue(bit_reader& br, std::int16_t block[64], int masks_count) {
            int coef_list[128];
            int mode_list[128];
            int list_start = 64, list_end = 64;
            int nz_coeff[64];
            int nz_coeff_count = 0;

            coef_list[list_end] =  4; mode_list[list_end++] = 0;
            coef_list[list_end] = 24; mode_list[list_end++] = 0;
            coef_list[list_end] = 44; mode_list[list_end++] = 0;
            coef_list[list_end] =  0; mode_list[list_end++] = 2;

            for (int mask = 1 << static_cast <int>(br.get_bits(3));
                 mask != 0; mask >>= 1) {
                for (int i = 0; i < nz_coeff_count; ++i) {
                    if (br.get_bit() == 0u) continue;
                    if (block[nz_coeff[i]] < 0) {
                        block[nz_coeff[i]] = static_cast <std::int16_t>(
                            block[nz_coeff[i]] - mask);
                    } else {
                        block[nz_coeff[i]] = static_cast <std::int16_t>(
                            block[nz_coeff[i]] + mask);
                    }
                    if (--masks_count < 0) return 0;
                }
                int list_pos = list_start;
                while (list_pos < list_end) {
                    if ((coef_list[list_pos] | mode_list[list_pos]) == 0 ||
                        br.get_bit() == 0u) {
                        ++list_pos;
                        continue;
                    }
                    int ccoef = coef_list[list_pos];
                    int mode  = mode_list[list_pos];
                    switch (mode) {
                        case 0:
                            coef_list[list_pos] = ccoef + 4;
                            mode_list[list_pos] = 1;
                            [[fallthrough]];
                        case 2: {
                            if (mode == 2) {
                                coef_list[list_pos] = 0;
                                mode_list[list_pos++] = 0;
                            }
                            for (int i = 0; i < 4; ++i, ++ccoef) {
                                if (br.get_bit() != 0u) {
                                    coef_list[--list_start] = ccoef;
                                    mode_list[  list_start] = 3;
                                } else {
                                    nz_coeff[nz_coeff_count++] = data::bink_scan[ccoef];
                                    const int sign = br.get_bit() ? -1 : 0;
                                    block[data::bink_scan[ccoef]] =
                                        static_cast <std::int16_t>((mask ^ sign) - sign);
                                    if (--masks_count < 0) return 0;
                                }
                            }
                            break;
                        }
                        case 1:
                            mode_list[list_pos] = 2;
                            for (int i = 0; i < 3; ++i) {
                                ccoef += 4;
                                coef_list[list_end]   = ccoef;
                                mode_list[list_end++] = 2;
                            }
                            break;
                        case 3: {
                            nz_coeff[nz_coeff_count++] = data::bink_scan[ccoef];
                            const int sign = br.get_bit() ? -1 : 0;
                            block[data::bink_scan[ccoef]] =
                                static_cast <std::int16_t>((mask ^ sign) - sign);
                            coef_list[list_pos] = 0;
                            mode_list[list_pos++] = 0;
                            if (--masks_count < 0) return 0;
                            break;
                        }
                    }
                }
            }
            return 0;
        }

        // 8x8 copy where source and destination may overlap (e.g. motion
        // vectors that point at a region that already includes pixels
        // we're about to overwrite). Buffer the read first.
        void put_pixels8x8_overlapped(std::uint8_t* dst, const std::uint8_t* src,
                                      std::size_t stride) noexcept {
            std::uint8_t tmp[64];
            for (std::size_t i = 0; i < 8; ++i) {
                std::memcpy(tmp + i * 8u, src + i * stride, 8);
            }
            for (std::size_t i = 0; i < 8; ++i) {
                std::memcpy(dst + i * stride, tmp + i * 8u, 8);
            }
        }

        // Plain 8x8 copy where overlap is impossible (the standard fast path).
        void put_pixels8x8(std::uint8_t* dst, const std::uint8_t* src,
                           std::size_t stride) noexcept {
            for (std::size_t i = 0; i < 8; ++i) {
                std::memcpy(dst + i * stride, src + i * stride, 8);
            }
        }

        // 8x8 fill with a constant value.
        void fill_block8x8(std::uint8_t* dst, std::uint8_t v,
                           std::size_t stride) noexcept {
            for (std::size_t i = 0; i < 8; ++i) std::memset(dst + i * stride, v, 8);
        }

        // 16x16 fill with a constant value (used by the SCALED FILL path).
        void fill_block16x16(std::uint8_t* dst, std::uint8_t v,
                             std::size_t stride) noexcept {
            for (std::size_t i = 0; i < 16; ++i) std::memset(dst + i * stride, v, 16);
        }

        // Single plane decode for the modern Bink variants. `prev_plane`
        // may be nullptr — if so, we read motion vectors from `dst_plane`
        // itself (matches ffmpeg's "first frame, no previous" path).
        result decode_plane(bit_reader& br,
                            bundle_set& bs,
                            std::uint8_t* dst_plane,
                            std::uint8_t* prev_plane,
                            unsigned int width, unsigned int height,
                            unsigned int stride,
                            char version) {
            // Bink encodes one extra bit of plane-level flag in version 'k':
            // a single "fill the whole plane" shortcut.
            if (version == 'k') {
                if (br.bits_left() < 1) {
                    return make_unexpected<error_type>(
                        "bink: 'k' plane-flag truncated");
                }
                if (br.get_bit() != 0u) {
                    if (br.bits_left() < 8) {
                        return make_unexpected<error_type>(
                            "bink: 'k' plane-fill truncated");
                    }
                    const auto fill = static_cast <std::uint8_t>(br.get_bits(8));
                    for (unsigned int y = 0; y < height; ++y) {
                        std::memset(dst_plane + y * stride, fill, width);
                    }
                    br.align_to_32();
                    return {};
                }
            }

            const unsigned int bw = (width  + 7u) >> 3u;
            const unsigned int bh = (height + 7u) >> 3u;
            bundle_set_init_lengths(bs, std::max(width, 8u), bw);
            if (auto r = bundle_set_read_trees(bs, br); !r) return r;

            const std::uint8_t* ref_plane = prev_plane ? prev_plane : dst_plane;
            const std::uint8_t* const ref_start = ref_plane;
            const std::uint8_t* const ref_end =
                ref_plane + (static_cast <std::size_t>(bh - 1u) * stride + (bw - 1u)) * 8u;

            // coordmap[i] = pixel offset within an 8x8 block (row-major
            // expansion of i) — used by RUN_BLOCK to write into a block
            // following bink_patterns[].
            int coordmap[64];
            for (int i = 0; i < 64; ++i) {
                coordmap[i] = (i & 7) + (i >> 3) * static_cast <int>(stride);
            }

            std::int32_t dctblock[64];
            std::int16_t residue_block[64];
            std::uint8_t ublock[64]; // scratch for SCALED 8x8→16x16

            for (unsigned int by = 0; by < bh; ++by) {
                if (auto r = read_block_types(br, bs.b[kSrcBlockTypes], version); !r) return r;
                if (auto r = read_block_types(br, bs.b[kSrcSubBlockTypes], version); !r) return r;
                if (auto r = read_colors(br, bs.b[kSrcColors], bs.colors, version); !r) return r;
                if (auto r = read_patterns(br, bs.b[kSrcPattern]); !r) return r;
                if (auto r = read_motion_values(br, bs.b[kSrcXOff]); !r) return r;
                if (auto r = read_motion_values(br, bs.b[kSrcYOff]); !r) return r;
                if (auto r = read_dcs(br, bs.b[kSrcIntraDc], 11u, false); !r) return r;
                if (auto r = read_dcs(br, bs.b[kSrcInterDc], 11u, true); !r) return r;
                if (auto r = read_runs(br, bs.b[kSrcRun]); !r) return r;

                std::uint8_t* dst  = dst_plane + 8u * by * stride;
                std::uint8_t* prev = (prev_plane ? prev_plane : dst_plane) + 8u * by * stride;

                for (unsigned int bx = 0; bx < bw; ++bx, dst += 8, prev += 8) {
                    int blk = bundle_get_value(bs, kSrcBlockTypes);
                    // 16x16 block on odd row/col means "covered by the
                    // already-decoded scaled block" — skip.
                    if (((by & 1u) || (bx & 1u)) && blk == kScaled) {
                        ++bx;
                        dst  += 8;
                        prev += 8;
                        continue;
                    }

                    auto motion_copy = [&](std::uint8_t* d) -> result {
                        const int xoff = bundle_get_value(bs, kSrcXOff);
                        const int yoff = bundle_get_value(bs, kSrcYOff);
                        const std::uint8_t* ref =
                            prev + xoff + yoff * static_cast <int>(stride);
                        if (ref < ref_start || ref > ref_end) {
                            return make_unexpected<error_type>(
                                "bink: motion ref out of bounds");
                        }
                        if (ref + 8 * stride < d || ref >= d + 8 * stride) {
                            put_pixels8x8(d, ref, stride);
                        } else {
                            put_pixels8x8_overlapped(d, ref, stride);
                        }
                        return {};
                    };

                    switch (blk) {
                        case kSkip:
                            put_pixels8x8(dst, prev, stride);
                            break;
                        case kScaled: {
                            const int sub = bundle_get_value(bs, kSrcSubBlockTypes);
                            switch (sub) {
                                case kRun: {
                                    if (br.bits_left() < 4) {
                                        return make_unexpected<error_type>(
                                            "bink: scaled-run scan truncated");
                                    }
                                    const auto* scan =
                                        data::bink_patterns[br.get_bits(4)];
                                    int i = 0;
                                    do {
                                        const int run = bundle_get_value(bs, kSrcRun) + 1;
                                        i += run;
                                        if (i > 64) {
                                            return make_unexpected<error_type>(
                                                "bink: scaled-run out of bounds");
                                        }
                                        if (br.bits_left() < 1) {
                                            return make_unexpected<error_type>(
                                                "bink: scaled-run flag truncated");
                                        }
                                        if (br.get_bit() != 0u) {
                                            const auto v = static_cast <std::uint8_t>(
                                                bundle_get_value(bs, kSrcColors));
                                            for (int j = 0; j < run; ++j) {
                                                ublock[*scan++] = v;
                                            }
                                        } else {
                                            for (int j = 0; j < run; ++j) {
                                                ublock[*scan++] = static_cast <std::uint8_t>(
                                                    bundle_get_value(bs, kSrcColors));
                                            }
                                        }
                                    } while (i < 63);
                                    if (i == 63) {
                                        ublock[*scan++] = static_cast <std::uint8_t>(
                                            bundle_get_value(bs, kSrcColors));
                                    }
                                    break;
                                }
                                case kIntra: {
                                    std::memset(dctblock, 0, sizeof(dctblock));
                                    dctblock[0] = bundle_get_value(bs, kSrcIntraDc);
                                    int coef_count = 0;
                                    int coef_idx[64];
                                    const int q = read_dct_coeffs(
                                        br, dctblock, data::bink_scan,
                                        coef_count, coef_idx, -1);
                                    if (q < 0) {
                                        return make_unexpected<error_type>(
                                            "bink: scaled-intra DCT failed");
                                    }
                                    unquantize_dct_coeffs(
                                        dctblock, data::bink_intra_quant[q],
                                        coef_count, coef_idx, data::bink_scan);
                                    idct_put(ublock, 8, dctblock);
                                    break;
                                }
                                case kFill: {
                                    const auto v = static_cast <std::uint8_t>(
                                        bundle_get_value(bs, kSrcColors));
                                    fill_block16x16(dst, v, stride);
                                    break;
                                }
                                case kPattern: {
                                    int col[2];
                                    for (int i = 0; i < 2; ++i) {
                                        col[i] = bundle_get_value(bs, kSrcColors);
                                    }
                                    for (int j = 0; j < 8; ++j) {
                                        int v = bundle_get_value(bs, kSrcPattern);
                                        for (int i = 0; i < 8; ++i, v >>= 1) {
                                            ublock[i + j * 8] =
                                                static_cast <std::uint8_t>(col[v & 1]);
                                        }
                                    }
                                    break;
                                }
                                case kRaw: {
                                    for (int j = 0; j < 8; ++j) {
                                        for (int i = 0; i < 8; ++i) {
                                            ublock[i + j * 8] = static_cast <std::uint8_t>(
                                                bundle_get_value(bs, kSrcColors));
                                        }
                                    }
                                    break;
                                }
                                default:
                                    return make_unexpected<error_type>(
                                        "bink: invalid sub-block type");
                            }
                            if (sub != kFill) {
                                scale_block(ublock, dst, stride);
                            }
                            // Step over the 16x16's 2nd column.
                            ++bx;
                            dst  += 8;
                            prev += 8;
                            break;
                        }
                        case kMotion:
                            if (auto r = motion_copy(dst); !r) return r;
                            break;
                        case kRun: {
                            if (br.bits_left() < 4) {
                                return make_unexpected<error_type>(
                                    "bink: run scan truncated");
                            }
                            const auto* scan = data::bink_patterns[br.get_bits(4)];
                            int i = 0;
                            do {
                                const int run = bundle_get_value(bs, kSrcRun) + 1;
                                i += run;
                                if (i > 64) {
                                    return make_unexpected<error_type>(
                                        "bink: run out of bounds");
                                }
                                if (br.bits_left() < 1) {
                                    return make_unexpected<error_type>(
                                        "bink: run flag truncated");
                                }
                                if (br.get_bit() != 0u) {
                                    const auto v = static_cast <std::uint8_t>(
                                        bundle_get_value(bs, kSrcColors));
                                    for (int j = 0; j < run; ++j) {
                                        dst[coordmap[*scan++]] = v;
                                    }
                                } else {
                                    for (int j = 0; j < run; ++j) {
                                        dst[coordmap[*scan++]] =
                                            static_cast <std::uint8_t>(
                                                bundle_get_value(bs, kSrcColors));
                                    }
                                }
                            } while (i < 63);
                            if (i == 63) {
                                dst[coordmap[*scan++]] = static_cast <std::uint8_t>(
                                    bundle_get_value(bs, kSrcColors));
                            }
                            break;
                        }
                        case kResidue: {
                            if (auto r = motion_copy(dst); !r) return r;
                            std::memset(residue_block, 0, sizeof(residue_block));
                            const int v = static_cast <int>(br.get_bits(7));
                            read_residue(br, residue_block, v);
                            add_pixels8(dst, residue_block, stride);
                            break;
                        }
                        case kIntra: {
                            std::memset(dctblock, 0, sizeof(dctblock));
                            dctblock[0] = bundle_get_value(bs, kSrcIntraDc);
                            int coef_count = 0;
                            int coef_idx[64];
                            const int q = read_dct_coeffs(
                                br, dctblock, data::bink_scan,
                                coef_count, coef_idx, -1);
                            if (q < 0) {
                                return make_unexpected<error_type>(
                                    "bink: intra DCT failed");
                            }
                            unquantize_dct_coeffs(
                                dctblock, data::bink_intra_quant[q],
                                coef_count, coef_idx, data::bink_scan);
                            idct_put(dst, stride, dctblock);
                            break;
                        }
                        case kFill: {
                            const auto v = static_cast <std::uint8_t>(
                                bundle_get_value(bs, kSrcColors));
                            fill_block8x8(dst, v, stride);
                            break;
                        }
                        case kInter: {
                            if (auto r = motion_copy(dst); !r) return r;
                            std::memset(dctblock, 0, sizeof(dctblock));
                            dctblock[0] = bundle_get_value(bs, kSrcInterDc);
                            int coef_count = 0;
                            int coef_idx[64];
                            const int q = read_dct_coeffs(
                                br, dctblock, data::bink_scan,
                                coef_count, coef_idx, -1);
                            if (q < 0) {
                                return make_unexpected<error_type>(
                                    "bink: inter DCT failed");
                            }
                            unquantize_dct_coeffs(
                                dctblock, data::bink_inter_quant[q],
                                coef_count, coef_idx, data::bink_scan);
                            idct_add(dst, stride, dctblock);
                            break;
                        }
                        case kPattern: {
                            int col[2];
                            for (int i = 0; i < 2; ++i) {
                                col[i] = bundle_get_value(bs, kSrcColors);
                            }
                            for (std::size_t i = 0; i < 8; ++i) {
                                int v = bundle_get_value(bs, kSrcPattern);
                                for (std::size_t j = 0; j < 8; ++j, v >>= 1) {
                                    dst[i * stride + j] =
                                        static_cast <std::uint8_t>(col[v & 1]);
                                }
                            }
                            break;
                        }
                        case kRaw: {
                            // RAW: 64 colors directly from the COLORS bundle.
                            for (std::size_t i = 0; i < 8; ++i) {
                                for (std::size_t j = 0; j < 8; ++j) {
                                    dst[i * stride + j] = static_cast <std::uint8_t>(
                                        bundle_get_value(bs, kSrcColors));
                                }
                            }
                            break;
                        }
                        default:
                            return make_unexpected<error_type>(
                                "bink: unknown block type");
                    }
                }
            }
            br.align_to_32();
            return {};
        }
    } // namespace

    void frame_state_init(frame_state& fs,
                          unsigned int width, unsigned int height) {
        auto plane_init = [&](plane_buffers& p) {
            const unsigned int aligned_w = (width + 7u) & ~7u;
            const unsigned int aligned_h = (height + 7u) & ~7u;
            p.width  = aligned_w;
            p.height = aligned_h;
            p.y.assign(static_cast <std::size_t>(aligned_w) * aligned_h, 0);
            // Chroma planes: 1/2 each dimension (4:2:0), then aligned to
            // 8 to fit the 8x8 block writer. Same formula as
            // `decode_frame` so the two agree on stride.
            const unsigned int cw = (width  + 1u) >> 1u;
            const unsigned int ch = (height + 1u) >> 1u;
            const unsigned int cw_aligned = (cw + 7u) & ~7u;
            const unsigned int ch_aligned = (ch + 7u) & ~7u;
            p.u.assign(static_cast <std::size_t>(cw_aligned) * ch_aligned, 0);
            p.v.assign(static_cast <std::size_t>(cw_aligned) * ch_aligned, 0);
        };
        plane_init(fs.a);
        plane_init(fs.b);
        fs.cur  = &fs.a;
        fs.prev = &fs.b;

        const unsigned int bw = (fs.cur->width  + 7u) >> 3u;
        const unsigned int bh = (fs.cur->height + 7u) >> 3u;
        bundle_set_init(fs.bundles, bw, bh);
        fs.frame_num = 0;
    }

    result decode_frame(frame_state& fs,
                        std::span <const std::uint8_t> video_bits,
                        const file_header& h) {
        // Swap cur/prev: previous frame is now read-only, current frame
        // gets overwritten. Bink versions > 'b' use the current frame
        // both as output and as the source for SKIP block reuse via
        // motion-vectoring against the previous frame.
        std::swap(fs.cur, fs.prev);

        bit_reader br{video_bits};

        // Alpha plane (rare, only when extradata flag 0x00100000 is set).
        if (h.has_alpha) {
            if (h.version >= 'i') {
                if (br.bits_left() < 32) {
                    return make_unexpected<error_type>("bink: alpha plane size truncated");
                }
                br.skip_bits(32);
            }
            // For now we don't expose the alpha plane to the surface —
            // skip-decode it into a scratch buffer of Y dimensions.
            // ffmpeg's bink_decode_plane takes plane_idx 3 here; we emulate
            // by routing to a temporary plane. Easiest: just decode into
            // y, then overwrite y with the real Y plane afterwards. That
            // still works because decode_plane mutates y but we re-decode
            // it next call.
            //
            // Defer alpha for now — the corpus has no alpha files.
            return make_unexpected<error_type>("bink: alpha plane not supported yet");
        }

        if (h.version >= 'i') {
            if (br.bits_left() < 32) {
                return make_unexpected<error_type>("bink: plane-prologue truncated");
            }
            br.skip_bits(32);
        }
        ++fs.frame_num;

        struct plane_layout {
            std::uint8_t* dst;
            std::uint8_t* prev;
            unsigned int  w, h, stride;
        };
        // Plane order in stream: Y, V, U (note swap — Bink writes V before U).
        // ffmpeg's `swap_planes` flag is true for versions 'h' and 'i';
        // the corpus shows our targets all use it. The simplest pragmatic
        // approach: decode in stream order, then assign to our (Y, U, V)
        // arrays accordingly.
        // ffmpeg: c->swap_planes = c->version >= 'h' (so 'h', 'i', 'k';
        // 'b', 'f', 'g' do NOT swap).
        const bool swap_planes = h.version >= 'h';
        plane_layout planes[3];
        // plane index 0: Y (always first)
        planes[0] = {fs.cur->y.data(), fs.prev->y.data(),
                     fs.cur->width, fs.cur->height, fs.cur->width};
        // Chroma is 4:2:0 subsampled — half-width and half-height,
        // rounded up: chroma_dim = (luma_dim + 1) >> 1. ffmpeg passes
        // `avctx->width >> 1` (i.e. integer-truncated half-width) which
        // matches our +1)>>1 form for both even and odd widths within
        // the natural Bink alignment rules.
        const unsigned int chroma_w = (h.width  + 1u) >> 1u;
        const unsigned int chroma_h = (h.height + 1u) >> 1u;
        const unsigned int chroma_stride = (chroma_w + 7u) & ~7u;
        if (swap_planes) {
            // stream order: Y, V, U
            planes[1] = {fs.cur->v.data(), fs.prev->v.data(),
                         chroma_w, chroma_h, chroma_stride};
            planes[2] = {fs.cur->u.data(), fs.prev->u.data(),
                         chroma_w, chroma_h, chroma_stride};
        } else {
            planes[1] = {fs.cur->u.data(), fs.prev->u.data(),
                         chroma_w, chroma_h, chroma_stride};
            planes[2] = {fs.cur->v.data(), fs.prev->v.data(),
                         chroma_w, chroma_h, chroma_stride};
        }

        for (int p = 0; p < 3; ++p) {
            if (auto r = decode_plane(
                    br, fs.bundles,
                    planes[p].dst,
                    fs.frame_num == 1u ? nullptr : planes[p].prev,
                    planes[p].w, planes[p].h, planes[p].stride,
                    h.version); !r) {
                return r;
            }
            if (br.bits_left() == 0) break;
        }
        return {};
    }
} // namespace bink
