#pragma once

#include <flc/types.hh>

#include <cstddef>
#include <cstdint>
#include <span>

namespace flc {
    // All decoders take a destination indexed8 framebuffer with explicit pitch
    // (bytes per row). Delta decoders (lc, ss2) read prior pixel state from the
    // same framebuffer; callers are responsible for managing the delta base.
    //
    // Palette decoders write a 256*3 = 768-byte RGB palette buffer.

    inline constexpr std::size_t kPaletteBytes = 256 * 3;

    [[nodiscard]] result decode_brun(std::span<const std::uint8_t> data,
                                     std::uint8_t* fb, std::size_t pitch,
                                     unsigned int width, unsigned int height);

    [[nodiscard]] result decode_lc(std::span<const std::uint8_t> data,
                                   std::uint8_t* fb, std::size_t pitch,
                                   unsigned int width, unsigned int height);

    [[nodiscard]] result decode_ss2(std::span<const std::uint8_t> data,
                                    std::uint8_t* fb, std::size_t pitch,
                                    unsigned int width, unsigned int height);

    [[nodiscard]] result decode_copy(std::span<const std::uint8_t> data,
                                     std::uint8_t* fb, std::size_t pitch,
                                     unsigned int width, unsigned int height);

    void decode_black(std::uint8_t* fb, std::size_t pitch,
                      unsigned int width, unsigned int height) noexcept;

    /// Apply a COLOR_64 chunk (FLI). Component values are in 0..63 and are
    /// scaled to 0..255 in the output palette.
    [[nodiscard]] result decode_color_64(std::span<const std::uint8_t> data,
                                         std::uint8_t* palette);

    /// Apply a COLOR_256 chunk (FLC). Component values are 0..255.
    [[nodiscard]] result decode_color_256(std::span<const std::uint8_t> data,
                                          std::uint8_t* palette);
} // namespace flc
