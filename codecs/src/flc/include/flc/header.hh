#pragma once

#include <flc/types.hh>

#include <cstddef>
#include <cstdint>
#include <span>

namespace flc {
    // ------------------------------------------------------------------------
    // Magic numbers
    // ------------------------------------------------------------------------

    inline constexpr std::uint16_t kMagicFli = 0xAF11; // 320x200, 70Hz timing
    inline constexpr std::uint16_t kMagicFlc = 0xAF12; // arbitrary size, ms timing

    inline constexpr std::uint16_t kFrameMagicStandard = 0xF1FA;
    inline constexpr std::uint16_t kFrameMagicPrefix   = 0xF100;
    inline constexpr std::uint16_t kFrameMagicCelData  = 0xF1E0;
    // Variant frame magic seen in some real-world files — same FRAME_TYPE
    // structure as 0xF1FA. ffmpeg accepts it; we do too for compatibility.
    inline constexpr std::uint16_t kFrameMagicVariant  = 0xF5FA;

    // Total file header size in bytes; we only parse the first kMinHeaderBytes.
    inline constexpr std::size_t kFileHeaderSize  = 128;
    inline constexpr std::size_t kMinHeaderBytes  = 24;
    inline constexpr std::size_t kFrameHeaderSize = 16;

    // ------------------------------------------------------------------------
    // file_header
    // ------------------------------------------------------------------------

    struct file_header {
        uint32_t size;          ///< total file size in bytes
        uint16_t magic;         ///< kMagicFli or kMagicFlc
        uint16_t frame_count;   ///< number of frames (excluding the ring frame)
        uint16_t width;
        uint16_t height;
        uint16_t depth;         ///< bits per pixel; always 8 in practice
        uint16_t flags;
        uint32_t speed_units;   ///< FLI: jiffies (1/70 s); FLC: milliseconds

        uint16_t reserved1;     /* Set to zero */
        uint32_t created;       /* Date of FLIC creation (FLC only) */
        uint32_t creator;       /* Serial number or compiler id (FLC only) */
        uint32_t updated;       /* Date of FLIC update (FLC only) */
        uint32_t updater;       /* Serial number (FLC only), see creator */
        uint16_t aspect_dx;     /* Width of square rectangle (FLC only) */
        uint16_t aspect_dy;     /* Height of square rectangle (FLC only) */
        uint16_t ext_flags;     /* EGI: flags for specific EGI extensions */
        uint16_t keyframes;     /* EGI: key-image frequency */
        uint16_t totalframes;   /* EGI: total number of frames (segments) */
        uint32_t req_memory;    /* EGI: maximum chunk size (uncompressed) — DWORD per spec */
        uint16_t max_regions;   /* EGI: max. number of regions in a CHK_REGION chunk */
        uint16_t transp_num;    /* EGI: number of transparent levels */
        uint8_t  reserved2[24]; /* Set to zero */
        uint32_t oframe1;       /* Offset to frame 1 (FLC only) */
        uint32_t oframe2;       /* Offset to frame 2 (FLC only) */
        uint8_t  reserved3[40]; /* Set to zero — BYTE[40] per spec, 40 bytes */

        [[nodiscard]] bool is_fli() const noexcept { return magic == kMagicFli; }
        [[nodiscard]] bool is_flc() const noexcept { return magic == kMagicFlc; }
    };

    /**
     * Parse the file header from the first bytes of an FLI/FLC file.
     * Validates the magic; returns an error on truncation or invalid magic.
     */
    [[nodiscard]] expected<file_header>
        parse_file_header(std::span<const std::uint8_t> data);

    // ------------------------------------------------------------------------
    // frame_header
    // ------------------------------------------------------------------------

    struct frame_header {
        std::uint32_t size;          ///< total chunk size including this header
        std::uint16_t magic;         ///< kFrameMagicStandard, _Prefix, or _CelData
        std::uint16_t sub_chunks;    ///< number of sub-chunks within this frame
        std::uint16_t delay;         ///< per-frame delay (ms); 0 = use file speed (Pro Motion ext.)
        std::int16_t  reserved;      ///< always zero
        std::uint16_t width;         ///< frame width override (0 = use file width) — EGI ext.
        std::uint16_t height;        ///< frame height override (0 = use file height) — EGI ext.
    };

    /**
     * Parse a 16-byte frame chunk header. Validates the magic.
     */
    [[nodiscard]] expected<frame_header>
        parse_frame_header(std::span<const std::uint8_t> data);
} // namespace flc
