#include <bink/bundle.hh>

#include <algorithm>
#include <cstring>

namespace bink {
    namespace {
        // Bink's RLE run lengths for block-type repetition (see
        // bink_rlelens in ffmpeg's bink.c). Indexed by (sym - 12) when
        // sym >= 12 in the BLOCK_TYPES decode.
        constexpr std::uint8_t kBlockTypeRunlens[4] = {4, 8, 12, 32};

        constexpr unsigned int log2u(unsigned int v) noexcept {
            // floor(log2(v)) for v > 0.
            unsigned int r = 0;
            while (v > 1) { v >>= 1u; ++r; }
            return r;
        }
    } // namespace

    void bundle_set_init(bundle_set& bs, unsigned int bw, unsigned int bh) {
        // ffmpeg allocates `bw * bh * 64` bytes per bundle (worst case
        // for the residue-DC-style streams that need 16-bit entries).
        // Clamp at the format-imposed maximum (7680×4800 → bw≤960, bh≤600,
        // cap ≤ 36 864 000 bytes per bundle) to keep gcc's
        // stringop-overflow analyzer happy.
        constexpr std::size_t kMaxCap = 64u * 960u * 600u;
        std::size_t cap =
            static_cast <std::size_t>(bw) *
            static_cast <std::size_t>(bh) * 64u;
        if (cap > kMaxCap) cap = kMaxCap;
        for (auto& b : bs.b) {
            b.data.assign(cap, 0);
            b.cur_dec = 0;
            b.cur_ptr = 0;
            b.active  = false;
        }
    }

    void bundle_set_init_lengths(bundle_set& bs, unsigned int width, unsigned int bw) noexcept {
        // Mirror init_lengths in bink.c: `len = av_log2((width >> 3) + 511) + 1`
        // for most bundles, with COLORS / PATTERN / RUN using different
        // multipliers.
        const unsigned int aligned_w = (width + 7u) & ~7u;
        const unsigned int len_a = log2u((aligned_w >> 3) + 511u) + 1u;
        const unsigned int len_b = log2u((aligned_w >> 4) + 511u) + 1u;
        const unsigned int len_c = log2u(bw * 64u + 511u) + 1u;
        const unsigned int len_p = log2u((bw << 3u) + 511u) + 1u;
        const unsigned int len_r = log2u(bw * 48u + 511u) + 1u;

        bs.b[kSrcBlockTypes].len    = len_a;
        bs.b[kSrcSubBlockTypes].len = len_b;
        bs.b[kSrcColors].len        = len_c;
        bs.b[kSrcIntraDc].len       = len_a;
        bs.b[kSrcInterDc].len       = len_a;
        bs.b[kSrcXOff].len          = len_a;
        bs.b[kSrcYOff].len          = len_a;
        bs.b[kSrcPattern].len       = len_p;
        bs.b[kSrcRun].len           = len_r;
    }

    result bundle_set_read_trees(bundle_set& bs, bit_reader& br) {
        // Tree reads happen in bundle-order with the COLORS bundle
        // expanded inline to its 16 side-trees + main tree. ffmpeg's
        // read_bundle is called per-bundle in this same order at the
        // top of bink_decode_plane (BLOCK_TYPES, SUB_BLOCK_TYPES,
        // COLORS, PATTERN, X_OFF, Y_OFF, RUN — INTRA_DC/INTER_DC are
        // skipped because their values are bit-fields rather than
        // tree-coded).
        for (unsigned int i = 0; i < kNumSrc; ++i) {
            if (i == kSrcIntraDc || i == kSrcInterDc) continue;
            if (i == kSrcColors) {
                for (unsigned int j = 0; j < 16; ++j) {
                    auto t = read_tree(br);
                    if (!t) return make_unexpected<error_type>(t.error());
                    bs.colors.tree_per_prev[j] = *t;
                }
                bs.colors.last_value = 0;
            }
            auto t = read_tree(br);
            if (!t) return make_unexpected<error_type>(t.error());
            bs.b[i].t = *t;
        }
        for (auto& b : bs.b) {
            b.cur_dec = 0;
            b.cur_ptr = 0;
            b.active  = true;
        }
        return {};
    }

    namespace {
        // Common preamble for "read a count field, bail out if zero".
        // Returns a positive count (in elements) on success, sets
        // `bd.active = false` and returns 0 on the "no more entries"
        // signal, or returns -1 on truncation. The caller is responsible
        // for verifying the data buffer can hold the count of entries.
        int begin_bundle(bit_reader& br, bundle& bd) {
            if (!bd.active || bd.cur_dec > bd.cur_ptr) {
                return 0;
            }
            if (br.bits_left() < bd.len) return -1;
            const auto t = br.get_bits(bd.len);
            if (t == 0) {
                bd.active = false;
                return 0;
            }
            return static_cast <int>(t);
        }

        // Helper: convert a tree-emitted nibble + sign into a signed value.
        int sym_to_signed(std::uint8_t v, bit_reader& br) {
            if (v == 0) return 0;
            const int sign = br.get_bit() ? -1 : 0;
            return (static_cast <int>(v) ^ sign) - sign;
        }
    } // namespace

    result read_runs(bit_reader& br, bundle& bd) {
        const auto t = begin_bundle(br, bd);
        if (t < 0) return make_unexpected<error_type>("bink: runs count truncated");
        if (t == 0) return {};
        if (bd.cur_dec + static_cast <std::size_t>(t) > bd.data.size()) {
            return make_unexpected<error_type>("bink: runs over-runs buffer");
        }
        if (br.bits_left() < 1) {
            return make_unexpected<error_type>("bink: runs sentinel truncated");
        }
        if (br.get_bit() != 0u) {
            const auto v = static_cast <std::uint8_t>(br.get_bits(4));
            std::memset(bd.data.data() + bd.cur_dec, v, static_cast <std::size_t>(t));
        } else {
            for (int i = 0; i < t; ++i) {
                bd.data[bd.cur_dec + static_cast <std::size_t>(i)] = tree_decode(bd.t, br);
            }
        }
        bd.cur_dec += static_cast <std::size_t>(t);
        return {};
    }

    result read_motion_values(bit_reader& br, bundle& bd) {
        const auto t = begin_bundle(br, bd);
        if (t < 0) return make_unexpected<error_type>("bink: motion count truncated");
        if (t == 0) return {};
        if (bd.cur_dec + static_cast <std::size_t>(t) > bd.data.size()) {
            return make_unexpected<error_type>("bink: motion over-runs buffer");
        }
        if (br.bits_left() < 1) {
            return make_unexpected<error_type>("bink: motion sentinel truncated");
        }
        if (br.get_bit() != 0u) {
            const auto v_raw = static_cast <std::uint8_t>(br.get_bits(4));
            int v = v_raw;
            if (v != 0) {
                v = sym_to_signed(v_raw, br);
            }
            const auto byte = static_cast <std::uint8_t>(v);
            std::memset(bd.data.data() + bd.cur_dec, byte, static_cast <std::size_t>(t));
        } else {
            for (int i = 0; i < t; ++i) {
                const auto sym = tree_decode(bd.t, br);
                int v = sym;
                if (v != 0) v = sym_to_signed(sym, br);
                bd.data[bd.cur_dec + static_cast <std::size_t>(i)] =
                    static_cast <std::uint8_t>(v);
            }
        }
        bd.cur_dec += static_cast <std::size_t>(t);
        return {};
    }

    result read_block_types(bit_reader& br, bundle& bd, char version) {
        auto t = begin_bundle(br, bd);
        if (t < 0) return make_unexpected<error_type>("bink: block-types count truncated");
        if (version == 'k' && t > 0) {
            t ^= 0xBB;
            if (t == 0) {
                bd.active = false;
                return {};
            }
        }
        if (t == 0) return {};
        if (bd.cur_dec + static_cast <std::size_t>(t) > bd.data.size()) {
            return make_unexpected<error_type>("bink: block-types over-runs buffer");
        }
        if (br.bits_left() < 1) {
            return make_unexpected<error_type>("bink: block-types sentinel truncated");
        }
        if (br.get_bit() != 0u) {
            const auto v = static_cast <std::uint8_t>(br.get_bits(4));
            std::memset(bd.data.data() + bd.cur_dec, v, static_cast <std::size_t>(t));
            bd.cur_dec += static_cast <std::size_t>(t);
        } else {
            // Decode with run-length expansion: symbols 12..15 mean
            // "repeat the last 0..11 value `bink_rlelens[v-12]` times".
            int last = 0;
            const auto end = bd.cur_dec + static_cast <std::size_t>(t);
            while (bd.cur_dec < end) {
                const auto v = tree_decode(bd.t, br);
                if (v < 12) {
                    last = v;
                    bd.data[bd.cur_dec++] = static_cast <std::uint8_t>(v);
                } else {
                    const int run = kBlockTypeRunlens[v - 12];
                    if (static_cast <std::size_t>(run) > end - bd.cur_dec) {
                        return make_unexpected<error_type>(
                            "bink: block-types run overruns end");
                    }
                    std::memset(bd.data.data() + bd.cur_dec,
                                static_cast <std::uint8_t>(last),
                                static_cast <std::size_t>(run));
                    bd.cur_dec += static_cast <std::size_t>(run);
                }
            }
        }
        return {};
    }

    result read_patterns(bit_reader& br, bundle& bd) {
        const auto t = begin_bundle(br, bd);
        if (t < 0) return make_unexpected<error_type>("bink: patterns count truncated");
        if (t == 0) return {};
        if (bd.cur_dec + static_cast <std::size_t>(t) > bd.data.size()) {
            return make_unexpected<error_type>("bink: patterns over-runs buffer");
        }
        for (int i = 0; i < t; ++i) {
            if (br.bits_left() < 2) {
                return make_unexpected<error_type>("bink: patterns truncated");
            }
            const auto lo = tree_decode(bd.t, br);
            const auto hi = tree_decode(bd.t, br);
            bd.data[bd.cur_dec++] =
                static_cast <std::uint8_t>(lo | (hi << 4));
        }
        return {};
    }

    result read_colors(bit_reader& br, bundle& bd, color_high_state& ch, char version) {
        const auto t = begin_bundle(br, bd);
        if (t < 0) return make_unexpected<error_type>("bink: colors count truncated");
        if (t == 0) return {};
        if (bd.cur_dec + static_cast <std::size_t>(t) > bd.data.size()) {
            return make_unexpected<error_type>("bink: colors over-runs buffer");
        }
        if (br.bits_left() < 1) {
            return make_unexpected<error_type>("bink: colors sentinel truncated");
        }
        auto fold_pre_i = [version](unsigned int v) -> unsigned int {
            if (version >= 'i') return v;
            // Pre-'i' files: sign-extend then offset by 0x80.
            const int sign = static_cast <int>(static_cast <int8_t>(v)) >> 7;
            const int low7 = static_cast <int>(v & 0x7Fu);
            const int folded = (low7 ^ sign) - sign;
            return static_cast <unsigned int>(folded + 0x80);
        };
        if (br.get_bit() != 0u) {
            ch.last_value = tree_decode(ch.tree_per_prev[ch.last_value], br);
            const auto lo = tree_decode(bd.t, br);
            const unsigned int v0 = (ch.last_value << 4) | lo;
            const auto v = static_cast <std::uint8_t>(fold_pre_i(v0));
            std::memset(bd.data.data() + bd.cur_dec, v, static_cast <std::size_t>(t));
            bd.cur_dec += static_cast <std::size_t>(t);
        } else {
            for (int i = 0; i < t; ++i) {
                if (br.bits_left() < 2) {
                    return make_unexpected<error_type>("bink: colors per-elem truncated");
                }
                ch.last_value = tree_decode(ch.tree_per_prev[ch.last_value], br);
                const auto lo = tree_decode(bd.t, br);
                const unsigned int v0 = (ch.last_value << 4) | lo;
                bd.data[bd.cur_dec++] =
                    static_cast <std::uint8_t>(fold_pre_i(v0));
            }
        }
        return {};
    }

    result read_dcs(bit_reader& br, bundle& bd, unsigned int start_bits, bool has_sign) {
        // DC values are 16-bit signed (or unsigned for has_sign=false).
        // Storage uses 2 bytes per entry; interpret as int16_t for
        // get_value. ffmpeg appends `len` 16-bit entries via the same
        // bundle data buffer, so we cast the cur_dec offset / 2 == count.
        const auto count = begin_bundle(br, bd);
        if (count < 0) return make_unexpected<error_type>("bink: dc count truncated");
        if (count == 0) return {};

        const std::size_t bytes_needed =
            static_cast <std::size_t>(count) * 2u;
        if (bd.cur_dec + bytes_needed > bd.data.size()) {
            return make_unexpected<error_type>("bink: dc over-runs buffer");
        }

        // First sample: read `start_bits - has_sign` bits + sign if has_sign.
        if (br.bits_left() < (start_bits - (has_sign ? 1u : 0u))) {
            return make_unexpected<error_type>("bink: dc seed truncated");
        }
        int v = static_cast <int>(br.get_bits(start_bits - (has_sign ? 1u : 0u)));
        if (has_sign && v != 0) {
            const int sign = br.get_bit() ? -1 : 0;
            v = (v ^ sign) - sign;
        }
        auto write_i16 = [&](int x) noexcept {
            const auto u = static_cast <std::uint16_t>(static_cast <std::int16_t>(x));
            bd.data[bd.cur_dec    ] = static_cast <std::uint8_t>(u & 0xFFu);
            bd.data[bd.cur_dec + 1] = static_cast <std::uint8_t>((u >> 8u) & 0xFFu);
            bd.cur_dec += 2;
        };
        write_i16(v);

        int remaining = count - 1;
        while (remaining > 0) {
            const int chunk = remaining < 8 ? remaining : 8;
            if (br.bits_left() < 4) {
                return make_unexpected<error_type>("bink: dc bsize truncated");
            }
            const unsigned int bsize = br.get_bits(4);
            if (bsize != 0) {
                for (int j = 0; j < chunk; ++j) {
                    if (br.bits_left() < bsize) {
                        return make_unexpected<error_type>("bink: dc value truncated");
                    }
                    int v2 = static_cast <int>(br.get_bits(bsize));
                    if (v2 != 0) {
                        const int sign = br.get_bit() ? -1 : 0;
                        v2 = (v2 ^ sign) - sign;
                    }
                    v += v2;
                    if (v < -32768 || v > 32767) {
                        return make_unexpected<error_type>("bink: dc out of range");
                    }
                    write_i16(v);
                }
            } else {
                for (int j = 0; j < chunk; ++j) write_i16(v);
            }
            remaining -= chunk;
        }
        return {};
    }

    int bundle_get_value(bundle_set& bs, src bundle_id) noexcept {
        auto& b = bs.b[bundle_id];
        // Sources with 1-byte unsigned representation:
        if (bundle_id == kSrcBlockTypes ||
            bundle_id == kSrcSubBlockTypes ||
            bundle_id == kSrcColors ||
            bundle_id == kSrcPattern ||
            bundle_id == kSrcRun) {
            return static_cast <int>(b.data[b.cur_ptr++]);
        }
        // Signed 1-byte sources (motion vectors):
        if (bundle_id == kSrcXOff || bundle_id == kSrcYOff) {
            const auto v = static_cast <std::int8_t>(b.data[b.cur_ptr++]);
            return static_cast <int>(v);
        }
        // 2-byte signed (DC):
        const auto lo = b.data[b.cur_ptr];
        const auto hi = b.data[b.cur_ptr + 1];
        const auto u = static_cast <std::uint16_t>(lo | (hi << 8u));
        const auto s = static_cast <std::int16_t>(u);
        b.cur_ptr += 2;
        return static_cast <int>(s);
    }
} // namespace bink
