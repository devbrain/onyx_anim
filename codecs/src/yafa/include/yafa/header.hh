#pragma once

#include <yafa/types.hh>

#include <cstdint>
#include <span>

namespace yafa {
    // ------------------------------------------------------------------------
    // INFO chunk — 14 bytes, big-endian. Mandatory.
    // ------------------------------------------------------------------------
    enum class frame_type : std::uint16_t {
        planar      = 0,
        planar_xpk  = 1,
        chunky_xpk  = 3,
        chunky8     = 4,
    };

    enum class delta_kind : std::uint8_t {
        none = 0,  ///< no delta compression
        byte = 1,  ///< 8-pixel-wide columns
        word = 2,  ///< 16-pixel-wide columns
        dlong = 3, ///< 32-pixel-wide columns ("long")
    };

    struct info {
        std::uint16_t width;       ///< must be a multiple of 16
        std::uint16_t height;
        std::uint16_t depth;       ///< 1..8
        std::uint16_t speed;       ///< video frames per anim frame (PAL: 50/speed → fps)
        std::uint16_t frames;
        frame_type    type;
        std::uint16_t flags;
        bool          ham;         ///< flags bit 0
        bool          dyn_palette; ///< flags bit 1 — palette stored after each frame
        bool          delta;       ///< flags bit 2 — delta-compressed
        delta_kind    delta_w;     ///< only meaningful when `delta`
    };

    [[nodiscard]] expected<info> parse_info(std::span<const std::uint8_t> data);

    // ------------------------------------------------------------------------
    // DRGB chunk — `loadrgb` structure: u16 count, u16 first, then count * 3
    // u32 colour components (R, G, B in the high byte of each longword).
    // Returns 8-bit-per-channel RGB triplets directly, sized count * 3 bytes.
    // ------------------------------------------------------------------------
    [[nodiscard]] expected<std::vector<std::uint8_t>>
        parse_drgb(std::span<const std::uint8_t> data);

    // ------------------------------------------------------------------------
    // PROF chunk — array of u32 cumulative offsets from the start of BODY,
    // one per frame. Size in bytes = chunk-size of PROF, count = size / 4.
    // ------------------------------------------------------------------------
    [[nodiscard]] expected<std::vector<std::uint32_t>>
        parse_prof(std::span<const std::uint8_t> data);
} // namespace yafa
