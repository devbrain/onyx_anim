#pragma once

#include <flc/types.hh>

#include <cstdint>
#include <span>

namespace flc {
    // FLI/FLC sub-chunk type codes.
    enum class sub_chunk_type : std::uint16_t {
        color_256 = 4,    ///< 256-level VGA palette (FLC)
        ss2       = 7,    ///< word-aligned RLE delta
        color_64  = 11,   ///< 64-level palette (FLI)
        lc        = 12,   ///< line-skip RLE delta
        black     = 13,   ///< clear framebuffer to color 0
        brun      = 15,   ///< byte-RLE keyframe
        copy      = 16,   ///< raw uncompressed pixels
        pstamp    = 18,   ///< postage-stamp preview (skip)
    };

    inline constexpr std::size_t kSubChunkHeaderSize = 6; // 4 bytes size + 2 bytes type

    struct sub_chunk_header {
        std::uint32_t size;          ///< total size including this 6-byte header
        sub_chunk_type type;
    };

    /**
     * Parse a 6-byte sub-chunk header from the start of `data`.
     * Returns the parsed header on success.
     */
    [[nodiscard]] expected<sub_chunk_header>
        parse_sub_chunk_header(std::span<const std::uint8_t> data);

    /**
     * Return a span over a sub-chunk's payload bytes (the bytes after the 6-byte
     * header, of length size - 6). Returns error on truncation.
     */
    [[nodiscard]] expected<std::span<const std::uint8_t>>
        sub_chunk_payload(std::span<const std::uint8_t> chunk_with_header);
} // namespace flc
