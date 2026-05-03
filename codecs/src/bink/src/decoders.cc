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

    // -------------------------------------------------------------------
    // BIK[b] codepath — older Bink variant with simpler bundles, fixed
    // bit-field widths, and per-file (not per-frame) Huffman tables.
    // -------------------------------------------------------------------

    namespace {
        // Bundles for the BIK[b] variant. Same set as ffmpeg's
        // `enum OldSources`. Order matters — it's the order they're
        // re-read at the top of each block row.
        enum binkb_src : unsigned int {
            kBSrcBlockTypes  = 0,
            kBSrcColors      = 1,
            kBSrcPattern     = 2,
            kBSrcXOff        = 3,
            kBSrcYOff        = 4,
            kBSrcIntraDc     = 5,
            kBSrcInterDc     = 6,
            kBSrcIntraQ      = 7,
            kBSrcInterQ      = 8,
            kBSrcInterCoefs  = 9,
            kBSrcCount       = 10,
        };

        constexpr std::uint8_t kBinkbBundleSizes[10] = {
            4, 8, 8, 5, 5, 11, 11, 4, 4, 7,
        };
        constexpr std::uint8_t kBinkbBundleSigned[10] = {
            0, 0, 0, 1, 1, 0, 1, 0, 0, 0,
        };

        // Per-tree-index quantisation matrices, computed once on first
        // use from the seed tables in data.hh. Same formula as ffmpeg's
        // `binkb_calc_quant()`.
        struct binkb_quant_tables {
            std::array <std::array <std::int32_t, 64>, 16> intra{};
            std::array <std::array <std::int32_t, 64>, 16> inter{};
        };

        const binkb_quant_tables& get_binkb_quant_tables() {
            static const binkb_quant_tables t = []() {
                binkb_quant_tables out{};
                std::uint8_t inv_bink_scan[64];
                static constexpr std::int64_t s_table[64] = {
                    1073741824, 1489322693, 1402911301, 1262586814, 1073741824,  843633538,  581104888,  296244703,
                    1489322693, 2065749918, 1945893874, 1751258219, 1489322693, 1170153332,  806015634,  410903207,
                    1402911301, 1945893874, 1832991949, 1649649171, 1402911301, 1102260336,  759250125,  387062357,
                    1262586814, 1751258219, 1649649171, 1484645031, 1262586814,  992008094,  683307060,  348346918,
                    1073741824, 1489322693, 1402911301, 1262586814, 1073741824,  843633538,  581104888,  296244703,
                     843633538, 1170153332, 1102260336,  992008094,  843633538,  662838617,  456571181,  232757969,
                     581104888,  806015634,  759250125,  683307060,  581104888,  456571181,  314491699,  160326478,
                     296244703,  410903207,  387062357,  348346918,  296244703,  232757969,  160326478,   81733730,
                };
                constexpr std::int64_t kC_shr12 = static_cast <std::int64_t>(1) << 18; // (1<<30)>>12
                for (int i = 0; i < 64; ++i) {
                    inv_bink_scan[data::bink_scan[i]] = static_cast <std::uint8_t>(i);
                }
                for (int j = 0; j < 16; ++j) {
                    for (int i = 0; i < 64; ++i) {
                        const int k = inv_bink_scan[i];
                        const std::int64_t num =
                            static_cast <std::int64_t>(data::binkb_num[j]);
                        const std::int64_t den =
                            static_cast <std::int64_t>(data::binkb_den[j]);
                        out.intra[static_cast <std::size_t>(j)][static_cast <std::size_t>(k)] =
                            static_cast <std::int32_t>(
                                static_cast <std::int64_t>(data::binkb_intra_seed[i]) *
                                s_table[i] * num /
                                (den * kC_shr12));
                        out.inter[static_cast <std::size_t>(j)][static_cast <std::size_t>(k)] =
                            static_cast <std::int32_t>(
                                static_cast <std::int64_t>(data::binkb_inter_seed[i]) *
                                s_table[i] * num /
                                (den * kC_shr12));
                    }
                }
                return out;
            }();
            return t;
        }

        // Read one BIK[b] bundle row: a count field of `kBinkbBundleSizes[i]+1`
        // (no — re-checking ffmpeg: uses `len = 13` for all bundles, the
        // actual bit width per ELEMENT is `kBinkbBundleSizes[i]`).
        // ffmpeg's `binkb_init_bundle` sets `len = 13` for the count
        // field, so each row's count is read as 13 bits regardless of
        // the per-element width.
        result binkb_read_bundle(bit_reader& br, bundle& bd, unsigned int bundle_idx) {
            // Stop if previous decode-into has already filled past the
            // read cursor — the bundle is "exhausted" until consumer
            // catches up.
            if (!bd.active || bd.cur_dec > bd.cur_ptr) return {};
            if (br.bits_left() < 13u) {
                return make_unexpected<error_type>("bink-b: count field truncated");
            }
            const auto count = static_cast <int>(br.get_bits(13));
            if (count == 0) {
                bd.active = false;
                return {};
            }
            const unsigned int bits = kBinkbBundleSizes[bundle_idx];
            const bool issigned = kBinkbBundleSigned[bundle_idx] != 0;
            const unsigned int mask = 1u << (bits - 1u);
            // 16-bit-storage bundles (DC) use 2 bytes per element.
            const std::size_t bytes_per = (bits > 8u) ? 2u : 1u;
            if (bd.cur_dec + static_cast <std::size_t>(count) * bytes_per
                > bd.data.size()) {
                return make_unexpected<error_type>("bink-b: bundle over-runs buffer");
            }
            if (br.bits_left() < static_cast <std::size_t>(count) * bits) {
                return make_unexpected<error_type>("bink-b: bundle data truncated");
            }
            if (bytes_per == 1u) {
                if (!issigned) {
                    for (int i = 0; i < count; ++i) {
                        bd.data[bd.cur_dec++] =
                            static_cast <std::uint8_t>(br.get_bits(bits));
                    }
                } else {
                    for (int i = 0; i < count; ++i) {
                        const auto v = static_cast <int>(br.get_bits(bits)) -
                                       static_cast <int>(mask);
                        bd.data[bd.cur_dec++] =
                            static_cast <std::uint8_t>(static_cast <std::int8_t>(v));
                    }
                }
            } else {
                for (int i = 0; i < count; ++i) {
                    int v = static_cast <int>(br.get_bits(bits));
                    if (issigned) v -= static_cast <int>(mask);
                    const auto u = static_cast <std::uint16_t>(
                        static_cast <std::int16_t>(v));
                    bd.data[bd.cur_dec    ] = static_cast <std::uint8_t>(u & 0xFFu);
                    bd.data[bd.cur_dec + 1] = static_cast <std::uint8_t>((u >> 8u) & 0xFFu);
                    bd.cur_dec += 2;
                }
            }
            return {};
        }

        int binkb_get_value(bundle_set& bs, binkb_src bundle_id) noexcept {
            auto& b = bs.b[bundle_id];
            const unsigned int bits = kBinkbBundleSizes[bundle_id];
            const bool issigned = kBinkbBundleSigned[bundle_id] != 0;
            if (bits <= 8u) {
                const auto byte = b.data[b.cur_ptr++];
                return issigned
                    ? static_cast <int>(static_cast <std::int8_t>(byte))
                    : static_cast <int>(byte);
            }
            const auto lo = b.data[b.cur_ptr];
            const auto hi = b.data[b.cur_ptr + 1];
            const auto u = static_cast <std::uint16_t>(lo | (hi << 8u));
            const auto s = static_cast <std::int16_t>(u);
            b.cur_ptr += 2;
            return static_cast <int>(s);
        }

        result binkb_decode_plane(bit_reader& br,
                                  bundle_set& bs,
                                  std::uint8_t* dst_plane,
                                  std::uint8_t* prev_plane,
                                  unsigned int width, unsigned int height,
                                  unsigned int stride,
                                  bool is_keyframe) {
            const unsigned int bw = (width  + 7u) >> 3u;
            const unsigned int bh = (height + 7u) >> 3u;

            // Quant matrices are computed once on first call (thread-
            // safe via Meyers' singleton).
            const auto& qt = get_binkb_quant_tables();

            // Reset bundles. ffmpeg's binkb_init_bundles does this at
            // the start of every plane, with len = 13 hard-coded.
            for (auto& b : bs.b) {
                b.cur_dec = 0;
                b.cur_ptr = 0;
                b.active  = true;
                b.len     = 13;
            }

            const std::uint8_t* ref_plane = prev_plane ? prev_plane : dst_plane;
            const std::uint8_t* const ref_start = ref_plane;
            const std::uint8_t* const ref_end =
                ref_plane + (static_cast <std::size_t>(bh - 1u) * stride + (bw - 1u)) * 8u;

            int coordmap[64];
            for (int i = 0; i < 64; ++i) {
                coordmap[i] = (i & 7) + (i >> 3) * static_cast <int>(stride);
            }

            const int ybias = is_keyframe ? -15 : 0;
            std::int32_t dctblock[64];
            std::int16_t residue_block[64];

            for (unsigned int by = 0; by < bh; ++by) {
                for (unsigned int i = 0; i < kBSrcCount; ++i) {
                    if (auto r = binkb_read_bundle(br, bs.b[i], i); !r) return r;
                }

                std::uint8_t* dst  = dst_plane + 8u * by * stride;

                for (unsigned int bx = 0; bx < bw; ++bx, dst += 8) {
                    const int blk = binkb_get_value(bs, kBSrcBlockTypes);

                    auto motion_only = [&](std::uint8_t* d) -> result {
                        const int xoff = binkb_get_value(bs, kBSrcXOff);
                        const int yoff = binkb_get_value(bs, kBSrcYOff) + ybias;
                        const std::uint8_t* ref =
                            d + xoff + yoff * static_cast <int>(stride);
                        // ffmpeg only copies when the reference is in
                        // bounds — out-of-bounds case leaves dst with
                        // whatever was there before (warning logged,
                        // execution continues).
                        if (ref >= ref_start && ref <= ref_end) {
                            if (ref + 8 * stride < d || ref >= d + 8 * stride) {
                                put_pixels8x8(d, ref, stride);
                            } else {
                                put_pixels8x8_overlapped(d, ref, stride);
                            }
                        }
                        return {};
                    };

                    switch (blk) {
                        case 0: // SKIP — leave previous-frame contents
                            break;
                        case 1: { // PATTERN+RUN
                            if (br.bits_left() < 4u) {
                                return make_unexpected<error_type>("bink-b: scan-idx truncated");
                            }
                            const auto* scan = data::bink_patterns[br.get_bits(4)];
                            unsigned int i = 0;
                            do {
                                const auto rbits = data::binkb_runbits[i];
                                if (br.bits_left() < 1u + rbits) {
                                    return make_unexpected<error_type>(
                                        "bink-b: pattern flag/run truncated");
                                }
                                const int mode = static_cast <int>(br.get_bit());
                                const int run = static_cast <int>(br.get_bits(rbits)) + 1;
                                if (i + static_cast <unsigned int>(run) > 64u) {
                                    return make_unexpected<error_type>(
                                        "bink-b: pattern run out of bounds");
                                }
                                if (mode != 0) {
                                    const auto v = static_cast <std::uint8_t>(
                                        binkb_get_value(bs, kBSrcColors));
                                    for (int j = 0; j < run; ++j) {
                                        dst[coordmap[*scan++]] = v;
                                    }
                                } else {
                                    for (int j = 0; j < run; ++j) {
                                        dst[coordmap[*scan++]] = static_cast <std::uint8_t>(
                                            binkb_get_value(bs, kBSrcColors));
                                    }
                                }
                                i += static_cast <unsigned int>(run);
                            } while (i < 63u);
                            if (i == 63u) {
                                dst[coordmap[*scan++]] = static_cast <std::uint8_t>(
                                    binkb_get_value(bs, kBSrcColors));
                            }
                            break;
                        }
                        case 2: { // INTRA DCT
                            std::memset(dctblock, 0, sizeof(dctblock));
                            dctblock[0] = binkb_get_value(bs, kBSrcIntraDc);
                            const int qp = binkb_get_value(bs, kBSrcIntraQ);
                            int coef_count = 0;
                            int coef_idx[64];
                            const int q = read_dct_coeffs(
                                br, dctblock, data::bink_scan,
                                coef_count, coef_idx, qp);
                            if (q < 0) {
                                return make_unexpected<error_type>("bink-b: intra DCT failed");
                            }
                            unquantize_dct_coeffs(
                                dctblock, qt.intra[static_cast <std::size_t>(q)].data(),
                                coef_count, coef_idx, data::bink_scan);
                            idct_put(dst, stride, dctblock);
                            break;
                        }
                        case 3: { // MOTION + RESIDUE
                            if (auto r = motion_only(dst); !r) return r;
                            std::memset(residue_block, 0, sizeof(residue_block));
                            const int v = binkb_get_value(bs, kBSrcInterCoefs);
                            read_residue(br, residue_block, v);
                            add_pixels8(dst, residue_block, stride);
                            break;
                        }
                        case 4: { // INTER DCT
                            if (auto r = motion_only(dst); !r) return r;
                            std::memset(dctblock, 0, sizeof(dctblock));
                            dctblock[0] = binkb_get_value(bs, kBSrcInterDc);
                            const int qp = binkb_get_value(bs, kBSrcInterQ);
                            int coef_count = 0;
                            int coef_idx[64];
                            const int q = read_dct_coeffs(
                                br, dctblock, data::bink_scan,
                                coef_count, coef_idx, qp);
                            if (q < 0) {
                                return make_unexpected<error_type>("bink-b: inter DCT failed");
                            }
                            unquantize_dct_coeffs(
                                dctblock, qt.inter[static_cast <std::size_t>(q)].data(),
                                coef_count, coef_idx, data::bink_scan);
                            idct_add(dst, stride, dctblock);
                            break;
                        }
                        case 5: { // FILL
                            const auto v = static_cast <std::uint8_t>(
                                binkb_get_value(bs, kBSrcColors));
                            fill_block8x8(dst, v, stride);
                            break;
                        }
                        case 6: { // PATTERN
                            int col[2];
                            for (int i = 0; i < 2; ++i) {
                                col[i] = binkb_get_value(bs, kBSrcColors);
                            }
                            for (std::size_t i = 0; i < 8; ++i) {
                                int v = binkb_get_value(bs, kBSrcPattern);
                                for (std::size_t j = 0; j < 8; ++j, v >>= 1) {
                                    dst[i * stride + j] =
                                        static_cast <std::uint8_t>(col[v & 1]);
                                }
                            }
                            break;
                        }
                        case 7: // MOTION ONLY
                            if (auto r = motion_only(dst); !r) return r;
                            break;
                        case 8: { // RAW — 64 bytes directly from COLORS
                            const auto& cb = bs.b[kBSrcColors];
                            if (cb.cur_ptr + 64 > cb.data.size()) {
                                return make_unexpected<error_type>(
                                    "bink-b: RAW exhausts COLORS bundle");
                            }
                            for (std::size_t i = 0; i < 8; ++i) {
                                std::memcpy(dst + i * stride,
                                            cb.data.data() + cb.cur_ptr + i * 8u, 8);
                            }
                            bs.b[kBSrcColors].cur_ptr += 64;
                            break;
                        }
                        default:
                            return make_unexpected<error_type>("bink-b: unknown block type");
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
            // Y plane initialised to 0 (= "absolute black" for limited-
            // range YUV). Chroma planes initialised to 128 (chroma
            // neutral), which matches ffmpeg's frame allocator and
            // matters specifically for BIK[b]'s SKIP / motion-bound-
            // exceeded blocks where the prior buffer content shows
            // through.
            p.y.assign(static_cast <std::size_t>(aligned_w) * aligned_h, 0);
            const unsigned int cw = (width  + 1u) >> 1u;
            const unsigned int ch = (height + 1u) >> 1u;
            const unsigned int cw_aligned = (cw + 7u) & ~7u;
            const unsigned int ch_aligned = (ch + 7u) & ~7u;
            p.u.assign(static_cast <std::size_t>(cw_aligned) * ch_aligned, 0x80);
            p.v.assign(static_cast <std::size_t>(cw_aligned) * ch_aligned, 0x80);
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

    result decode_frame_b(frame_state& fs,
                          std::span <const std::uint8_t> video_bits,
                          const file_header& h) {
        // BIK[b] flow: ffmpeg's decode_frame calls ff_reget_buffer for
        // version<='b' and then av_frame_ref into `frame`, so cur and
        // prev are the SAME buffer. The decoder writes deltas in place.
        // We emulate by always pointing prev → cur (same plane buffers).
        bit_reader br{video_bits};
        if (br.bits_left() == 0u) {
            return make_unexpected<error_type>("bink-b: empty packet");
        }
        ++fs.frame_num;
        const bool is_key = (fs.frame_num == 1u);

        plane_buffers* p = fs.cur;
        const unsigned int chroma_w = (h.width  + 1u) >> 1u;
        const unsigned int chroma_h = (h.height + 1u) >> 1u;
        const unsigned int chroma_stride = (chroma_w + 7u) & ~7u;

        // BIK[b] doesn't swap planes — stream order is Y, U, V.
        struct plane_layout {
            std::uint8_t* dst;
            unsigned int  w, h, stride;
        };
        plane_layout planes[3] = {
            {p->y.data(), p->width,  p->height, p->width},
            {p->u.data(), chroma_w, chroma_h,  chroma_stride},
            {p->v.data(), chroma_w, chroma_h,  chroma_stride},
        };
        for (int pi = 0; pi < 3; ++pi) {
            if (auto r = binkb_decode_plane(
                    br, fs.bundles,
                    planes[pi].dst, planes[pi].dst,
                    planes[pi].w, planes[pi].h, planes[pi].stride,
                    is_key); !r) {
                return r;
            }
            if (br.bits_left() == 0) break;
        }
        return {};
    }
} // namespace bink
