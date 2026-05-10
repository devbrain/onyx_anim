#pragma once

// 8x8 integer IDCT used by Bink's INTRA_BLOCK / INTER_BLOCK paths,
// ported from ffmpeg's libavcodec/binkdsp.c. Standard separable IDCT
// with fixed-point integer constants (Q11). The "put" form writes to
// `dst` directly, "add" form adds to existing pixels (clamped to 0..255).
//
// Plus two helpers used by the SCALED 16x16 path:
//   scale_block — duplicate an 8x8 source into a 16x16 destination
//   add_pixels8 — add an int16 8x8 block to an 8-bit destination

#include <cstddef>
#include <cstdint>

namespace bink {
    // Single-block IDCT producing 8-bit output. `block` is consumed.
    void idct_put(std::uint8_t* dst, std::size_t stride, std::int32_t* block) noexcept;

    // Single-block IDCT, accumulating into 8-bit output (with clamping).
    void idct_add(std::uint8_t* dst, std::size_t stride, std::int32_t* block) noexcept;

    // Scale an 8x8 source into a 16x16 destination (each input pixel
    // becomes a 2x2 quad in the output).
    void scale_block(const std::uint8_t* src, std::uint8_t* dst, std::size_t stride) noexcept;

    // Add a residue block (int16) to an 8-bit destination, no clamping
    // (matches ffmpeg's add_pixels8_c — wraparound is the documented
    // behaviour even though it's mathematically suspect).
    void add_pixels8(std::uint8_t* dst, std::int16_t* block, std::size_t stride) noexcept;
} // namespace bink
