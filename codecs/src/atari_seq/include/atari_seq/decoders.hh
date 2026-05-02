#pragma once

#include <atari_seq/header.hh>
#include <atari_seq/types.hh>

#include <cstddef>
#include <cstdint>
#include <span>

namespace atari_seq {
    /**
     * Decode the cel palette (16 ST 0x0RGB words) into a 256*3 RGB triplet
     * buffer. Entries beyond `nColors` are left zero.
     */
    void decode_palette(const std::uint16_t st_palette[16],
                        unsigned int n_colors,
                        std::uint8_t* rgb_out_768) noexcept;

    /**
     * Apply a SEQ frame's data block to the planar framebuffer.
     *
     * The framebuffer is organised as N independent bitplanes of size
     * `scanline_stride * height` bytes each, concatenated; `bitplane_stride`
     * gives the offset between consecutive planes. `scanline_stride` is bytes
     * per row (typically `(width + 7) / 8`).
     *
     * The frame describes a region (x, y, w, h) within the framebuffer. The
     * iteration order matches Randelshofer's `SEQDeltaFrame.decodeXxx`:
     *
     *   for each plane b in 0..planes-1:
     *     for each x_group of 16 pixels along [x .. x+w):
     *       for each row in [y .. y+h):
     *         emit one 16-bit word at (x_group, row, plane)
     *
     * Words are written to the framebuffer; in OP_Copy mode they replace the
     * existing contents of the rect (after a zero-fill); in OP_XOR mode they
     * XOR against the existing contents (delta against the previous frame).
     *
     * For SM_word_rle, the first word of each opcode group is a count:
     *   - high bit set    → literal: copy abs(count) words verbatim
     *   - high bit clear  → repeat the next single word `count` times
     * (Same convention as Randelshofer's reader; matches the file format.)
     */
    [[nodiscard]] result apply_frame(std::span<const std::uint8_t> data,
                                     std::uint8_t* fb_planar,
                                     std::size_t scanline_stride,
                                     std::size_t bitplane_stride,
                                     unsigned int planes,
                                     unsigned int frame_width,
                                     unsigned int x_offset,
                                     unsigned int y_offset,
                                     unsigned int rect_width,
                                     unsigned int rect_height,
                                     operation op,
                                     storage   sm);

    /**
     * Convert N-bitplane planar bitmap to chunky 8-bit indexed pixels.
     *
     * Planes are read from `planar` at offsets `b * bitplane_stride`. Output
     * `chunky` has `chunky_stride` bytes per row, each pixel one byte holding
     * the index into the per-frame palette.
     */
    void planar_to_chunky(const std::uint8_t* planar,
                          std::size_t scanline_stride,
                          std::size_t bitplane_stride,
                          unsigned int planes,
                          std::uint8_t* chunky,
                          std::size_t chunky_stride,
                          unsigned int width,
                          unsigned int height) noexcept;
} // namespace atari_seq
