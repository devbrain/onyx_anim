#pragma once

// 16 fixed Bink VLC trees, decoded via a 7-bit lookup table. Each entry of
// the table maps the next 7 LSB-first bits to (symbol, code_length); the
// caller consumes `code_length` bits after the lookup.
//
// Tree structure (from ffmpeg's libavcodec/bink.c bink_init_vlcs):
//   bink_tree_bits[i][j] = the L-bit code value emitted by symbol j
//   bink_tree_lens[i][j] = bit length L (max 7 for the standard tree set)
// Codes appear in the bit stream LSB-first (matches Bink's BITSTREAM_READER_LE
// convention) — so reading `L` LSB-first bits and matching against
// `bink_tree_bits[i][*]` directly recovers the symbol.

#include <bink/bit_reader.hh>

#include <array>
#include <cstdint>

namespace bink {
    inline constexpr unsigned int kVlcTableBits = 7;

    struct vlc_entry {
        std::uint8_t sym;
        std::uint8_t len;
    };

    // 16 trees × 128 entries each. Filled at lib_bink load time via a
    // call to `vlc_tables_init()` (idempotent).
    using vlc_tables_t = std::array <std::array <vlc_entry, 1u << kVlcTableBits>, 16>;
    [[nodiscard]] const vlc_tables_t& vlc_tables() noexcept;

    // Decode one VLC code from `br` against tree `tree_idx` (0..15) and
    // return the 4-bit symbol. The hot path: peek 7 bits, look up, skip
    // the consumed length. No bounds checks at the per-code level —
    // callers verify the per-bundle bit budget before kicking off a
    // batch.
    inline std::uint8_t vlc_get(bit_reader& br, unsigned int tree_idx) noexcept {
        const auto& tab = vlc_tables()[tree_idx];
        const auto idx = br.peek_bits(kVlcTableBits);
        const auto& e = tab[idx];
        br.skip_bits(e.len);
        return e.sym;
    }
} // namespace bink
