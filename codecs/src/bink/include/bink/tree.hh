#pragma once

// Bink's per-frame Huffman tree representation. Each tree describes a 4-bit
// alphabet (16 leaves). Decoding a value:
//   1. read a prefix code via one of the 16 fixed VLCs (selected by
//      `vlc_num`); the result is a leaf index 0..15.
//   2. translate the leaf index through `syms[]` to the actual symbol.
//
// Tree construction (one of three variants based on the leading bits):
//   - vlc_num == 0  → identity (syms[i] = i)
//   - "literal list": next 1-bit flag set → 3-bit length-1, then `len`
//     explicit 4-bit symbols, with the rest of the alphabet appended in
//     ascending order
//   - "merge tower": next 1-bit flag clear → 2-bit pair-up depth, build
//     the symbol order by repeatedly merging adjacent runs based on
//     bits read from the stream

#include <bink/bit_reader.hh>
#include <bink/types.hh>
#include <bink/vlc.hh>

#include <array>
#include <cstdint>

namespace bink {
    struct tree {
        unsigned int             vlc_num = 0;
        std::array <std::uint8_t, 16> syms{}; // leaf-index → symbol
    };

    // Read a single tree from the bit stream into `out`. Returns failure
    // on truncated stream (caller must check bits_left() before invoking
    // for cheap recovery; the inner read does not bounds-check every bit).
    expected<tree> read_tree(bit_reader& br);

    // Decode one symbol from the bit stream using the supplied tree.
    inline std::uint8_t tree_decode(const tree& t, bit_reader& br) noexcept {
        // The all-zero "identity tree" path is hit very often in
        // well-compressed clips; short-circuit to avoid the table lookup.
        return t.syms[vlc_get(br, t.vlc_num)];
    }
} // namespace bink
