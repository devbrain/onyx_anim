#pragma once

#include <anim/types.hh>

#include <cstdint>
#include <span>
#include <vector>

namespace anim {
    // ------------------------------------------------------------------------
    // BMHD — bitmap header (20 bytes, big-endian)
    // ------------------------------------------------------------------------

    enum class masking : std::uint8_t {
        none                  = 0,
        has_mask              = 1,
        has_transparent_color = 2,
        lasso                 = 3,
    };

    enum class compression : std::uint8_t {
        none      = 0,
        byte_run1 = 1,
    };

    struct bmhd {
        std::uint16_t width;
        std::uint16_t height;
        std::int16_t  x_origin;
        std::int16_t  y_origin;
        std::uint8_t  planes;
        masking       mask;
        compression   compress;
        std::uint8_t  pad1;
        std::uint16_t transparent;
        std::uint8_t  x_aspect;
        std::uint8_t  y_aspect;
        std::int16_t  page_width;
        std::int16_t  page_height;
    };

    // ------------------------------------------------------------------------
    // ANHD — animation header (40 bytes, big-endian)
    // ------------------------------------------------------------------------

    struct anhd {
        std::uint8_t  operation;     ///< 0=keyframe, 5=byte vertical delta, 7,8 = vertical, etc.
        std::uint8_t  mask;          ///< plane mask (which planes the delta touches)
        std::uint16_t w;
        std::uint16_t h;
        std::int16_t  x;
        std::int16_t  y;
        std::uint32_t abstime;       ///< jiffies from start of anim
        std::uint32_t reltime;       ///< jiffies from previous frame
        std::uint8_t  interleave;    ///< 0 (defaults to 2 = XOR-against-2-frames-back), 1, 2
        std::uint8_t  pad0;
        std::uint32_t bits;          ///< operation-specific flags
        std::uint8_t  pad[16];
    };

    // ------------------------------------------------------------------------
    // CAMG — Amiga viewport mode flags
    // ------------------------------------------------------------------------

    inline constexpr std::uint32_t kCamgEhb  = 0x0080;
    inline constexpr std::uint32_t kCamgHam  = 0x0800;

    // ------------------------------------------------------------------------
    // Parsers (operate on raw chunk payload bytes)
    // ------------------------------------------------------------------------

    [[nodiscard]] expected<bmhd> parse_bmhd(std::span<const std::uint8_t> data);
    [[nodiscard]] expected<anhd> parse_anhd(std::span<const std::uint8_t> data);

    /// Parse a CMAP chunk into RGB triplets (1 byte each); returns at most 256 entries.
    [[nodiscard]] expected<std::vector<std::uint8_t>>
        parse_cmap(std::span<const std::uint8_t> data);

    /// Parse a CAMG chunk (4 bytes BE) returning the viewport-mode flags.
    [[nodiscard]] expected<std::uint32_t>
        parse_camg(std::span<const std::uint8_t> data);
} // namespace anim
