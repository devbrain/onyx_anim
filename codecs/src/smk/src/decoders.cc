#include <smk/decoders.hh>

#include <algorithm>
#include <cstring>

namespace smk {
    namespace {
        // Per-block run lengths, indexed by bits 2..7 of the TYPE-tree code
        // (6 bits → 64 entries). Mirrors ffmpeg's `block_runs[]`.
        constexpr int kBlockRuns[64] = {
            1, 2, 3, 4, 5, 6, 7, 8,
            9, 10, 11, 12, 13, 14, 15, 16,
            17, 18, 19, 20, 21, 22, 23, 24,
            25, 26, 27, 28, 29, 30, 31, 32,
            33, 34, 35, 36, 37, 38, 39, 40,
            41, 42, 43, 44, 45, 46, 47, 48,
            49, 50, 51, 52, 53, 54, 55, 56,
            57, 58, 59, 128, 256, 512, 1024, 2048
        };

        enum block_type : std::uint32_t {
            kBlkMono = 0,
            kBlkFull = 1,
            kBlkSkip = 2,
            kBlkFill = 3,
        };

        // Smacker's built-in 64-entry palette. Used as a translation table
        // for the "new entry" branch of the palette delta opcode stream:
        // the encoder emits a 6-bit index into this table for each colour
        // channel of a fresh palette entry.
        constexpr std::uint8_t kSmkPal[64] = {
            0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C,
            0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C,
            0x41, 0x45, 0x49, 0x4D, 0x51, 0x55, 0x59, 0x5D,
            0x61, 0x65, 0x69, 0x6D, 0x71, 0x75, 0x79, 0x7D,
            0x82, 0x86, 0x8A, 0x8E, 0x92, 0x96, 0x9A, 0x9E,
            0xA2, 0xA6, 0xAA, 0xAE, 0xB2, 0xB6, 0xBA, 0xBE,
            0xC3, 0xC7, 0xCB, 0xCF, 0xD3, 0xD7, 0xDB, 0xDF,
            0xE3, 0xE7, 0xEB, 0xEF, 0xF3, 0xF7, 0xFB, 0xFF
        };
    } // namespace

    // -------------------------------------------------------------------

    expected<video_trees>
    build_video_trees(std::span <const std::uint8_t> trees_blob,
                      const file_header& h) {
        video_trees vt{};
        bit_reader br{trees_blob};

        auto t = read_big_tree(br, h.mmap_size);
        if (!t) return make_unexpected<error_type>(t.error());
        vt.mmap = std::move(*t);

        t = read_big_tree(br, h.mclr_size);
        if (!t) return make_unexpected<error_type>(t.error());
        vt.mclr = std::move(*t);

        t = read_big_tree(br, h.full_size);
        if (!t) return make_unexpected<error_type>(t.error());
        vt.full = std::move(*t);

        t = read_big_tree(br, h.type_size);
        if (!t) return make_unexpected<error_type>(t.error());
        vt.type = std::move(*t);

        return vt;
    }

    // -------------------------------------------------------------------

    result decode_video_frame(video_trees& trees,
                              std::span <const std::uint8_t> frame_bits,
                              std::uint32_t width,
                              std::uint32_t height,
                              bool is_smk4,
                              std::uint8_t* out) {
        // Reset the recency cache for all four trees on each frame —
        // recency state does not carry between frames.
        big_tree_reset_last(trees.mmap);
        big_tree_reset_last(trees.mclr);
        big_tree_reset_last(trees.full);
        big_tree_reset_last(trees.type);

        bit_reader br{frame_bits};

        const std::uint32_t bw = width >> 2;
        const std::uint32_t bh = height >> 2;
        const std::uint32_t blocks = bw * bh;
        const std::size_t   stride = width;

        std::uint32_t blk = 0;
        while (blk < blocks) {
            auto type_code = big_tree_decode(trees.type, br);
            if (!type_code) {
                return make_unexpected<error_type>(type_code.error());
            }
            const std::uint32_t code = *type_code;
            const std::uint32_t btype = code & 0x3u;
            const std::uint32_t run_idx = (code >> 2u) & 0x3Fu;
            std::int32_t run = kBlockRuns[run_idx];

            switch (btype) {
                case kBlkMono: {
                    while (run-- > 0 && blk < blocks) {
                        auto clr = big_tree_decode(trees.mclr, br);
                        if (!clr) return make_unexpected<error_type>(clr.error());
                        auto map = big_tree_decode(trees.mmap, br);
                        if (!map) return make_unexpected<error_type>(map.error());
                        const std::uint8_t hi =
                            static_cast <std::uint8_t>((*clr >> 8u) & 0xFFu);
                        const std::uint8_t lo =
                            static_cast <std::uint8_t>(*clr & 0xFFu);
                        std::uint32_t m = *map;
                        std::uint8_t* o = out +
                            static_cast <std::size_t>(blk / bw) * stride * 4u +
                            static_cast <std::size_t>(blk % bw) * 4u;
                        for (int row = 0; row < 4; ++row) {
                            o[0] = (m & 1u) ? hi : lo;
                            o[1] = (m & 2u) ? hi : lo;
                            o[2] = (m & 4u) ? hi : lo;
                            o[3] = (m & 8u) ? hi : lo;
                            m >>= 4u;
                            o += stride;
                        }
                        ++blk;
                    }
                    break;
                }
                case kBlkFull: {
                    int mode = 0;
                    if (is_smk4) {
                        if (br.bits_left() < 1) {
                            return make_unexpected<error_type>(
                                "smk: SMK4 FULL mode bits truncated");
                        }
                        if (br.get_bit() != 0u) {
                            mode = 1;
                        } else {
                            if (br.bits_left() < 1) {
                                return make_unexpected<error_type>(
                                    "smk: SMK4 FULL mode bits truncated");
                            }
                            if (br.get_bit() != 0u) mode = 2;
                        }
                    }
                    while (run-- > 0 && blk < blocks) {
                        std::uint8_t* o = out +
                            static_cast <std::size_t>(blk / bw) * stride * 4u +
                            static_cast <std::size_t>(blk % bw) * 4u;
                        switch (mode) {
                            case 0: {
                                // 8 codes per block: 4 rows, 2 codes per row.
                                // Each code is `i1 | i2 << 8`. ffmpeg writes
                                // `pix2` to o[2..3] and `pix1` to o[0..1] —
                                // i.e. high pair (right half) first.
                                for (int row = 0; row < 4; ++row) {
                                    auto p2 = big_tree_decode(trees.full, br);
                                    if (!p2) return make_unexpected<error_type>(p2.error());
                                    auto p1 = big_tree_decode(trees.full, br);
                                    if (!p1) return make_unexpected<error_type>(p1.error());
                                    o[2] = static_cast <std::uint8_t>(*p2 & 0xFFu);
                                    o[3] = static_cast <std::uint8_t>((*p2 >> 8u) & 0xFFu);
                                    o[0] = static_cast <std::uint8_t>(*p1 & 0xFFu);
                                    o[1] = static_cast <std::uint8_t>((*p1 >> 8u) & 0xFFu);
                                    o += stride;
                                }
                                break;
                            }
                            case 1: {
                                // 2 codes total, each driving a 2x2 region
                                // of "left-pair / right-pair" pixels stamped
                                // across two rows.
                                auto pa = big_tree_decode(trees.full, br);
                                if (!pa) return make_unexpected<error_type>(pa.error());
                                {
                                    const auto pix = *pa;
                                    const std::uint8_t l =
                                        static_cast <std::uint8_t>(pix & 0xFFu);
                                    const std::uint8_t hi =
                                        static_cast <std::uint8_t>((pix >> 8u) & 0xFFu);
                                    o[0] = l;  o[1] = l;  o[2] = hi; o[3] = hi;
                                    o += stride;
                                    o[0] = l;  o[1] = l;  o[2] = hi; o[3] = hi;
                                    o += stride;
                                }
                                auto pb = big_tree_decode(trees.full, br);
                                if (!pb) return make_unexpected<error_type>(pb.error());
                                {
                                    const auto pix = *pb;
                                    const std::uint8_t l =
                                        static_cast <std::uint8_t>(pix & 0xFFu);
                                    const std::uint8_t hi =
                                        static_cast <std::uint8_t>((pix >> 8u) & 0xFFu);
                                    o[0] = l;  o[1] = l;  o[2] = hi; o[3] = hi;
                                    o += stride;
                                    o[0] = l;  o[1] = l;  o[2] = hi; o[3] = hi;
                                }
                                break;
                            }
                            case 2: {
                                // 4 codes total in two halves. Each half: read
                                // pix2 then pix1, write pix1 to bytes 0-1 and
                                // pix2 to bytes 2-3, replicated across two
                                // rows.
                                for (int half = 0; half < 2; ++half) {
                                    auto p2 = big_tree_decode(trees.full, br);
                                    if (!p2) return make_unexpected<error_type>(p2.error());
                                    auto p1 = big_tree_decode(trees.full, br);
                                    if (!p1) return make_unexpected<error_type>(p1.error());
                                    o[0] = static_cast <std::uint8_t>(*p1 & 0xFFu);
                                    o[1] = static_cast <std::uint8_t>((*p1 >> 8u) & 0xFFu);
                                    o[2] = static_cast <std::uint8_t>(*p2 & 0xFFu);
                                    o[3] = static_cast <std::uint8_t>((*p2 >> 8u) & 0xFFu);
                                    o += stride;
                                    o[0] = static_cast <std::uint8_t>(*p1 & 0xFFu);
                                    o[1] = static_cast <std::uint8_t>((*p1 >> 8u) & 0xFFu);
                                    o[2] = static_cast <std::uint8_t>(*p2 & 0xFFu);
                                    o[3] = static_cast <std::uint8_t>((*p2 >> 8u) & 0xFFu);
                                    o += stride;
                                }
                                break;
                            }
                        }
                        ++blk;
                    }
                    break;
                }
                case kBlkSkip: {
                    while (run-- > 0 && blk < blocks) {
                        ++blk;
                    }
                    break;
                }
                case kBlkFill: {
                    const std::uint8_t mode_byte =
                        static_cast <std::uint8_t>((code >> 8u) & 0xFFu);
                    while (run-- > 0 && blk < blocks) {
                        std::uint8_t* o = out +
                            static_cast <std::size_t>(blk / bw) * stride * 4u +
                            static_cast <std::size_t>(blk % bw) * 4u;
                        for (int row = 0; row < 4; ++row) {
                            o[0] = mode_byte;
                            o[1] = mode_byte;
                            o[2] = mode_byte;
                            o[3] = mode_byte;
                            o += stride;
                        }
                        ++blk;
                    }
                    break;
                }
                default:
                    return make_unexpected<error_type>("smk: invalid block type");
            }
        }
        return {};
    }

    // -------------------------------------------------------------------
    // Palette delta opcodes
    // -------------------------------------------------------------------
    //
    // Per ffmpeg's smacker_read_packet:
    //   byte0 = chunk_size_in_dwords (consumed bytes = 4*byte0)
    //   then a stream of opcodes until 256 entries have been processed:
    //     0x80-bit set: skip (count = (b & 0x7F) + 1) palette entries
    //     0x40-bit set: copy with offset
    //                   src_off = next_byte
    //                   count   = (b & 0x3F) + 1
    //                   copy `count` entries from old palette starting
    //                   at `src_off`
    //     else (b & 0xC0 == 0): three "new entry" bytes follow this:
    //                   r = smk_pal[b]
    //                   g = smk_pal[next_byte & 0x3F]
    //                   b = smk_pal[next_byte & 0x3F]
    // The opcodes only cover the low 6 bits of the colour index; ffmpeg
    // explicitly masks with `& 0x3F` for the green/blue indices.

    expected<std::size_t>
    apply_palette_block(std::span <const std::uint8_t> data,
                        std::uint8_t* palette_768) {
        if (data.empty()) {
            return make_unexpected<error_type>("smk: palette block empty");
        }
        const std::size_t total_bytes = static_cast <std::size_t>(data[0]) * 4u;
        if (total_bytes == 0u || total_bytes > data.size()) {
            return make_unexpected<error_type>("smk: palette block size out of range");
        }

        // Snapshot old palette for the copy-with-offset opcode.
        std::uint8_t old_pal[768];
        std::memcpy(old_pal, palette_768, 768);

        std::size_t cursor = 1; // start after the size byte
        const std::size_t end = total_bytes;
        std::size_t entries_done = 0;

        while (entries_done < 256) {
            if (cursor >= end) break;
            const std::uint8_t b = data[cursor++];
            if (b & 0x80) {
                // skip palette entries — leave them as-is in palette_768
                const std::size_t n = static_cast <std::size_t>(b & 0x7F) + 1u;
                entries_done += n;
                if (entries_done >= 256u) break;
            } else if (b & 0x40) {
                // copy from old palette
                if (cursor >= end) {
                    return make_unexpected<error_type>(
                        "smk: palette copy-offset truncated");
                }
                const std::size_t src_off = data[cursor++];
                std::size_t count = static_cast <std::size_t>(b & 0x3F) + 1u;
                if (src_off + count > 256u) {
                    return make_unexpected<error_type>(
                        "smk: palette copy out of range");
                }
                const std::uint8_t* src = old_pal + src_off * 3u;
                while (count-- > 0 && entries_done < 256u) {
                    std::uint8_t* p = palette_768 + entries_done * 3u;
                    p[0] = src[0]; p[1] = src[1]; p[2] = src[2];
                    src += 3;
                    ++entries_done;
                }
            } else {
                // new entry: 3 indexed colour bytes via smk_pal[]
                if (cursor + 1 >= end) {
                    return make_unexpected<error_type>(
                        "smk: palette new-entry truncated");
                }
                const std::uint8_t g_idx = data[cursor++];
                const std::uint8_t b_idx = data[cursor++];
                std::uint8_t* p = palette_768 + entries_done * 3u;
                p[0] = kSmkPal[b];
                p[1] = kSmkPal[g_idx & 0x3Fu];
                p[2] = kSmkPal[b_idx & 0x3Fu];
                ++entries_done;
            }
        }
        return total_bytes;
    }

    // -------------------------------------------------------------------
    // Audio
    // -------------------------------------------------------------------

    namespace {
        // Convert a single small_tree decode wrapped against an in-progress
        // bit_reader into either the leaf value (when the tree is a real
        // VLC) or the constant when the tree degenerated.
        std::uint8_t read_small_value(const small_tree& t, bit_reader& br,
                                      bool& failed) {
            if (failed) return 0;
            if (t.has_single) return t.single_value;
            auto v = small_tree_decode(t, br);
            if (!v) { failed = true; return 0; }
            return *v;
        }
    } // namespace

    result decode_audio_chunk(const audio_track_info& track,
                              std::span <const std::uint8_t> chunk,
                              std::vector <std::uint8_t>& out) {
        if (track.unsupported) {
            return make_unexpected<error_type>(
                "smk: BinkAud / DCT audio not supported");
        }
        if (!track.packed) {
            // Raw payload: copy verbatim. Format: 8-bit unsigned for 8-bit
            // tracks (PCM_U8), 16-bit signed LE for 16-bit tracks.
            out.assign(chunk.begin(), chunk.end());
            return {};
        }

        if (chunk.size() < 4u) {
            return make_unexpected<error_type>("smk: audio chunk smaller than unp_size");
        }
        const std::uint32_t unp_size =
            static_cast <std::uint32_t>(chunk[0]) |
            (static_cast <std::uint32_t>(chunk[1]) << 8u) |
            (static_cast <std::uint32_t>(chunk[2]) << 16u) |
            (static_cast <std::uint32_t>(chunk[3]) << 24u);
        if (unp_size > (1u << 24u)) {
            return make_unexpected<error_type>("smk: audio unp_size too large");
        }

        bit_reader br{chunk.subspan(4)};
        if (br.bits_left() < 1u) {
            return make_unexpected<error_type>("smk: audio bits truncated");
        }
        if (br.get_bit() == 0u) {
            // "Sound: no data" — emit a silent buffer of unp_size bytes.
            // ffmpeg doesn't emit any samples in this branch (returns 1
            // and sets got_frame_ptr=0). We do the same — empty output.
            out.clear();
            return {};
        }
        if (br.bits_left() < 2u) {
            return make_unexpected<error_type>("smk: audio header truncated");
        }
        const unsigned int stereo = br.get_bit();
        const unsigned int bits   = br.get_bit();

        if ((stereo != 0u) != track.stereo ||
            (bits != 0u)   != track.bits16) {
            // Per-chunk flags must match the track header. ffmpeg returns
            // an error in this case.
            return make_unexpected<error_type>(
                "smk: per-chunk audio flags disagree with track header");
        }

        // Build per-channel small trees: 2 trees for mono-8, 4 trees for
        // stereo-8 or mono-16, 8 trees for stereo-16. Wait — ffmpeg uses
        // `1 << (bits + stereo)` trees: 8-bit mono = 1, 8-bit stereo = 2,
        // 16-bit mono = 2, 16-bit stereo = 4.
        const unsigned int n_trees = 1u << (bits + stereo);
        std::array <small_tree, 4> trees;
        for (unsigned int i = 0; i < n_trees; ++i) {
            // ffmpeg audio sequence per tree:
            //   skip_bits1            (the encoder's leading marker —
            //                          always 1 in well-formed audio,
            //                          ffmpeg doesn't verify, just skips)
            //   smacker_decode_tree   (recursive build, no present bit)
            //   skip_bits1            (trailer)
            // We must NOT use read_small_tree here because that wrapper
            // also consumes a present bit — the audio per-tree shape
            // already separates the leading bit. read_small_tree_bare
            // skips that step.
            if (br.bits_left() < 1u) {
                return make_unexpected<error_type>("smk: audio tree skip truncated");
            }
            br.skip_bits(1);
            auto t = read_small_tree_bare(br);
            if (!t) return make_unexpected<error_type>(t.error());
            trees[i] = std::move(*t);
        }

        const unsigned int nch = stereo + 1u;
        unsigned int sample_bytes = (bits ? 2u : 1u);
        if ((unp_size % (nch * sample_bytes)) != 0u) {
            return make_unexpected<error_type>(
                "smk: audio unp_size not divisible by sample size");
        }

        out.assign(static_cast <std::size_t>(unp_size), 0u);
        bool failed = false;

        if (bits) {
            // 16-bit decoding. Predictors: read 16 LSB-first bits per
            // channel, in REVERSE channel order (i = stereo .. 0).
            std::array <std::uint32_t, 2> pred{0, 0};
            for (int i = static_cast <int>(stereo); i >= 0; --i) {
                if (br.bits_left() < 16u) {
                    return make_unexpected<error_type>(
                        "smk: 16-bit predictor truncated");
                }
                // ffmpeg: pred[i] = av_bswap16(get_bits(16)).
                // get_bits(16) under LSB-first reader returns the LE u16
                // of the next two bytes; bswap16 turns it into the BE u16
                // (= (byte0 << 8) | byte1). Equivalently: read first byte
                // into the high half, second into the low half.
                const auto byte0 = br.get_bits(8);
                const auto byte1 = br.get_bits(8);
                pred[static_cast <std::size_t>(i)] =
                    (byte0 << 8u) | byte1;
            }
            // Emit predictors as the first sample of each channel.
            std::uint16_t* samples =
                reinterpret_cast <std::uint16_t*>(out.data());
            const std::size_t total_samples =
                static_cast <std::size_t>(unp_size) / 2u;
            std::size_t emitted = 0;
            for (unsigned int i = 0; i <= stereo; ++i) {
                if (emitted >= total_samples) break;
                samples[emitted++] = static_cast <std::uint16_t>(pred[i]);
            }
            // Decode the remaining samples.
            // ffmpeg's per-step idx mapping: for stereo, channel = (i & 1)
            // and the two trees of the channel are at indices 2*ch and
            // 2*ch+1 (low byte and high byte). For mono, channel = 0 and
            // trees 0 / 1.
            for (std::size_t i = emitted; i < total_samples; ++i) {
                const unsigned int ch =
                    stereo ? static_cast <unsigned int>(i & 1u) : 0u;
                const unsigned int idx_lo = ch * 2u;
                const unsigned int idx_hi = idx_lo + 1u;
                const auto lo = read_small_value(trees[idx_lo], br, failed);
                const auto hi = read_small_value(trees[idx_hi], br, failed);
                if (failed) {
                    return make_unexpected<error_type>("smk: 16-bit audio bits exhausted");
                }
                const std::uint32_t delta =
                    static_cast <std::uint32_t>(lo) |
                    (static_cast <std::uint32_t>(hi) << 8u);
                pred[ch] = (pred[ch] + delta) & 0xFFFFu;
                samples[i] = static_cast <std::uint16_t>(pred[ch]);
            }
        } else {
            // 8-bit decoding. Predictors are 8-bit raw bytes per channel,
            // again in reverse channel order.
            std::array <std::uint32_t, 2> pred{0, 0};
            for (int i = static_cast <int>(stereo); i >= 0; --i) {
                if (br.bits_left() < 8u) {
                    return make_unexpected<error_type>(
                        "smk: 8-bit predictor truncated");
                }
                pred[static_cast <std::size_t>(i)] = br.get_bits(8);
            }
            std::size_t emitted = 0;
            for (unsigned int i = 0; i <= stereo; ++i) {
                if (emitted >= unp_size) break;
                out[emitted++] = static_cast <std::uint8_t>(pred[i]);
            }
            for (std::size_t i = emitted; i < unp_size; ++i) {
                const unsigned int ch =
                    stereo ? static_cast <unsigned int>(i & 1u) : 0u;
                const auto delta = read_small_value(trees[ch], br, failed);
                if (failed) {
                    return make_unexpected<error_type>("smk: 8-bit audio bits exhausted");
                }
                pred[ch] = (pred[ch] + delta) & 0xFFu;
                out[i] = static_cast <std::uint8_t>(pred[ch]);
            }
        }
        return {};
    }
} // namespace smk
