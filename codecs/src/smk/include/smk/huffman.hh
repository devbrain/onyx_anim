#pragma once

// Smacker Huffman tree decoders.
//
// Two variants:
//
// `small_tree` — leaves carry an 8-bit value. Tree is encoded recursively:
//                bit 0 = leaf (followed by 8-bit value), bit 1 = internal
//                node (left subtree, then right subtree). Used for the
//                low/high byte sub-trees nested inside each large tree, and
//                for per-channel audio trees.
//
// `big_tree`   — leaves carry a 16-bit value (composed via two small_tree
//                lookups, bytes low|hi<<8) plus a 3-entry escape mechanism
//                that mutates leaf cells in-place to act as a "repeat last
//                code" facility. This is what the per-frame video block
//                decoder reads (MMAP / MCLR / FULL / TYPE).
//
// Both representations are flat `vector<uint32_t>` arrays with ffmpeg's
// `SMK_NODE` high-bit convention so the inner walker can stay branch-light:
//   internal: kSmkNodeBit | jump_distance_to_right_child_minus_one
//   leaf:     value
// The walker increments the cursor after every bit. On bit=1 it first jumps
// by `jump_distance` (= size-of-left-subtree), then increments — landing on
// the first cell of the right subtree.

#include <smk/bit_reader.hh>
#include <smk/types.hh>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace smk {
    inline constexpr std::uint32_t kSmkNodeBit = 0x80000000u;

    // ---------- small tree (8-bit leaves) -------------------------------

    struct small_tree {
        // If `single_value` is set, the tree degenerated to a single leaf
        // (or was absent entirely). `nodes` is unused in that case.
        std::vector <std::uint32_t> nodes;
        std::uint8_t single_value = 0;
        bool         has_single   = true;
        bool         present      = false; // false = stream's "skip tree" bit was 0
    };

    // Decode a small tree from the bit stream. Format (top-level wrapper):
    //   1 bit   present
    //   if present:
    //     <recursive tree>
    //     1 bit  trailing
    // Returns failure on malformed input.
    expected<small_tree> read_small_tree(bit_reader& br);

    // Bare variant used by the audio decoder. Format:
    //   <recursive tree>
    //   1 bit  trailing
    // No present-bit prefix — the audio path's outer loop handles that
    // separately (the encoder always asserts the present bit, so ffmpeg
    // just `skip_bits1`s past it without checking).
    expected<small_tree> read_small_tree_bare(bit_reader& br);

    // Walk a small tree against the bit stream. Caller must hold a tree
    // returned by `read_small_tree`; behaviour is undefined on a default-
    // constructed tree. Returns the leaf value.
    expected<std::uint8_t> small_tree_decode(const small_tree& t,
                                             bit_reader& br);

    // ---------- big tree (16-bit leaves + 3-escape recency) -------------

    struct big_tree {
        std::vector <std::uint32_t> nodes;     // flat tree
        std::array <int, 3>         last{-1, -1, -1}; // escape leaf indices
        std::array <std::uint32_t, 3> escapes{0, 0, 0}; // 16-bit escape codepoints
        bool                        present = false;
        std::uint32_t               single_value = 0; // when entire tree absent
    };

    // Construct a big tree from the bit stream as found at the start of
    // each per-frame extradata blob. `expected_size_bytes` is the size
    // field stored at the top of the extradata for this tree (mmap_size /
    // mclr_size / full_size / type_size); it caps the value-array length.
    //
    // Layout:
    //   1 bit   present
    //   if !present: tree degenerates to constant 0
    //   else:
    //     <small tree #0>  (low byte)
    //     <small tree #1>  (high byte)
    //     16 bits escape[0]
    //     16 bits escape[1]
    //     16 bits escape[2]
    //     <recursive bigtree>  (leaves emit i1 | i2<<8 via the two small trees)
    //     1 bit  trailing
    expected<big_tree> read_big_tree(bit_reader& br,
                                     std::uint32_t expected_size_bytes);

    // Reset the 3 escape-leaf cells to 0. Called at the start of every
    // video frame (and conceptually at the start of audio chunks, though
    // audio uses small trees only).
    void big_tree_reset_last(big_tree& t) noexcept;

    // Walk the big tree, emit the leaf value, and update the recency
    // cache. The leaf cell value is what's returned — escape leaves
    // therefore emit the most-recent / second / third most-recent value.
    expected<std::uint32_t> big_tree_decode(big_tree& t, bit_reader& br);
} // namespace smk
