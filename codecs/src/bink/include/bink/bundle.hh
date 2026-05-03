#pragma once

// Bink decodes each plane via a small set of "bundles" — independent
// per-frame streams of decoded values that the per-block walker
// consumes in order. Each bundle has its own per-frame Huffman tree,
// a length field (count of entries to decode, expressed in bits via
// `len`) read at every plane row, and a backing buffer.
//
// Source IDs for the modern (`BIK[fghi]`) variant. `binkb` uses a
// different set; we don't model it here yet.

#include <bink/bit_reader.hh>
#include <bink/tree.hh>
#include <bink/types.hh>

#include <array>
#include <cstdint>
#include <vector>

namespace bink {
    enum src : unsigned int {
        kSrcBlockTypes    = 0, // 8x8 block types
        kSrcSubBlockTypes = 1, // 16x16 block types (subset of 8x8 block types)
        kSrcColors        = 2, // pixel values
        kSrcPattern       = 3, // 8-bit values for 2-colour pattern fill
        kSrcXOff          = 4, // signed X motion
        kSrcYOff          = 5, // signed Y motion
        kSrcIntraDc       = 6, // 16-bit DCT DC values for intra blocks
        kSrcInterDc       = 7, // 16-bit DCT DC values for inter blocks (signed)
        kSrcRun           = 8, // run lengths for fill blocks
        kNumSrc           = 9,
    };

    struct bundle {
        // Persistent across frames: storage allocated at decoder open.
        std::vector <std::uint8_t> data;

        // Per-frame state:
        unsigned int len = 0; // bit width of the "count" field
        tree         t{};     // Huffman tree (recreated each row)

        // Cursors index into `data`. `cur_dec` is the position the
        // decoder writes to; `cur_ptr` is what the per-block walker
        // reads from. `inactive` marks bundles whose tree decoded a
        // 0-count "stop" — get_value should not advance past existing
        // contents in that case.
        std::size_t  cur_dec = 0;
        std::size_t  cur_ptr = 0;
        bool         active  = true; // false = "no more entries this row"
    };

    // The colors bundle has an extra side-channel of 16 trees keyed by
    // the previous high-nibble. Stored separately because it doesn't fit
    // the per-bundle struct cleanly.
    struct color_high_state {
        std::array <tree, 16> tree_per_prev;
        unsigned int          last_value = 0;
    };

    // Bundle storage sized for the larger of the two layouts (BIK[b]
    // needs 10 slots; modern Bink uses 9). The decoder only touches
    // the active subset for the path it's running.
    struct bundle_set {
        static constexpr unsigned int kCapacity = 10;
        std::array <bundle, kCapacity> b;
        color_high_state               colors;
    };

    // Allocate per-bundle storage for a frame of `bw × bh` 8x8 blocks.
    void bundle_set_init(bundle_set& bs, unsigned int bw, unsigned int bh);

    // Set per-bundle `len` field for the current plane width.
    void bundle_set_init_lengths(bundle_set& bs, unsigned int width, unsigned int bw) noexcept;

    // Read the per-frame Huffman trees for every bundle (called once
    // at the start of each plane decode). For `kSrcColors`, also reads
    // the 16 sub-trees for high-nibble decoding.
    result bundle_set_read_trees(bundle_set& bs, bit_reader& br);

    // Per-row dispatch — at the top of each block row, every active
    // bundle re-reads its leading "count" field and decodes that many
    // values. Each call returns success/failure.
    result read_runs(bit_reader& br, bundle& bd);
    result read_motion_values(bit_reader& br, bundle& bd);
    result read_block_types(bit_reader& br, bundle& bd, char version);
    result read_patterns(bit_reader& br, bundle& bd);
    result read_colors(bit_reader& br, bundle& bd, color_high_state& ch, char version);
    result read_dcs(bit_reader& br, bundle& bd, unsigned int start_bits, bool has_sign);

    // Per-block read accessors mirroring ffmpeg's get_value.
    int bundle_get_value(bundle_set& bs, src bundle_id) noexcept;
} // namespace bink
