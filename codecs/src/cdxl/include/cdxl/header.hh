#pragma once

#include <cdxl/types.hh>

#include <cstddef>
#include <cstdint>
#include <span>

namespace cdxl {
    // ------------------------------------------------------------------------
    // CDXL chunk header — 32 bytes, big-endian.
    //
    // Each frame is one self-contained chunk: header + colormap + bitmap +
    // audio, packed back-to-back on disk. Chunks are typically constant size
    // within a file but the format permits variation.
    //
    // Reference: iffanimplay's cdxl.{cpp,h} and the original CDXL.guide.
    // ------------------------------------------------------------------------

    inline constexpr std::size_t kChunkHeaderSize = 32;

    enum class kind : std::uint8_t {
        custom   = 0x00,
        standard = 0x01,
        special  = 0x02,
    };

    enum class video_encoding : std::uint8_t {
        rgb       = 0x00,  // 1..8 bpp indexed (with cmap), or >=24 bpp direct
        ham       = 0x01,  // HAM6 or HAM8
        yuv       = 0x02,
        avm_dctv  = 0x03,  // Amiga AVM/DCTV — not handled
    };

    enum class pixel_orientation : std::uint8_t {
        bit_planar  = 0x00,
        byte_planar = 0x20,
        chunky      = 0x40,
        bit_line    = 0x80,
        byte_line   = 0xC0,
    };

    struct chunk_header {
        kind              type;
        video_encoding    venc;
        pixel_orientation por;
        bool              stereo;       // info byte 0x10
        std::uint32_t     csize_cur;    // current chunk size, including header
        std::uint32_t     csize_prev;
        std::uint32_t     fnumber;      // frame number as stored in the chunk
        std::uint16_t     width;
        std::uint16_t     height;
        std::uint8_t      planes;       // bitplanes; 0..8 for indexed or HAM
        std::uint16_t     cmap_bytes;   // colormap size in bytes (each entry is 2 BE)
        std::uint16_t     audio_bytes;  // raw sound size in bytes
    };

    /// Parse a 32-byte chunk header. Validates type/venc/por enums but
    /// performs no cross-field consistency checks beyond that.
    [[nodiscard]] expected<chunk_header>
        parse_chunk_header(std::span<const std::uint8_t> data);

    /// Per-row bitplane stride in bytes for the bit-planar layout: each
    /// plane row is rounded up to the nearest 16-bit word so that 16-bit
    /// writes never split a byte boundary (Amiga hardware convention).
    [[nodiscard]] inline constexpr std::size_t
    bitplane_pitch(unsigned int width) noexcept {
        return ((static_cast<std::size_t>(width) + 15u) / 16u) * 2u;
    }

    /// Convert one CDXL colormap entry (12-bit RGB packed in a u16) into
    /// 8-bit-per-channel R, G, B. Layout per spec:
    ///   bits 11..8  = R   (or 0 if it's the leading nibble for HAM6)
    ///   bits  7..4  = G
    ///   bits  3..0  = B
    /// Each component is replicated into the upper nibble (× 17) to fill 8
    /// bits — the standard Amiga 4-bit → 8-bit expansion.
    inline constexpr void rgb12_to_888(std::uint16_t entry,
                                       std::uint8_t& r,
                                       std::uint8_t& g,
                                       std::uint8_t& b) noexcept {
        const auto r4 = static_cast<std::uint8_t>((entry >> 8) & 0x0F);
        const auto g4 = static_cast<std::uint8_t>((entry >> 4) & 0x0F);
        const auto b4 = static_cast<std::uint8_t>(entry & 0x0F);
        r = static_cast<std::uint8_t>((r4 << 4) | r4);
        g = static_cast<std::uint8_t>((g4 << 4) | g4);
        b = static_cast<std::uint8_t>((b4 << 4) | b4);
    }
} // namespace cdxl
