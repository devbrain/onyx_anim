#pragma once

#include <atari_seq/types.hh>

#include <cstddef>
#include <cstdint>
#include <span>

namespace atari_seq {
    // ------------------------------------------------------------------------
    // Magic numbers
    // ------------------------------------------------------------------------

    inline constexpr std::uint16_t kMagicCyber   = 0xFEDB; // Cyber Paint
    inline constexpr std::uint16_t kMagicFlicker = 0xFEDC; // Flicker

    inline constexpr std::size_t kFileHeaderSize = 128;
    inline constexpr std::size_t kCelHeaderSize  = 128;

    // 0x0RGB → 8-bit RGB by 3-bit replicate-shift: (c<<5)|(c<<2)|(c>>1).
    inline constexpr std::uint8_t scale_st_3_to_8(unsigned c) noexcept {
        c &= 0x7u;
        return static_cast<std::uint8_t>(((c << 5) | (c << 2) | (c >> 1)) & 0xFFu);
    }

    // ------------------------------------------------------------------------
    // file_header — first 128 bytes of a SEQ file
    // ------------------------------------------------------------------------

    struct file_header {
        std::uint16_t magic;        ///< kMagicCyber (0xFEDB) or kMagicFlicker (0xFEDC)
        std::uint16_t version;      ///< 0
        std::uint32_t frame_count;  ///< number of frames; offsets table that follows
                                    ///< has frame_count u32 entries
        std::uint16_t speed;        ///< delay between frames; units: timebase 6000 ns
                                    ///< (so 1 unit = 6 µs)

        [[nodiscard]] bool is_cyber()   const noexcept { return magic == kMagicCyber; }
        [[nodiscard]] bool is_flicker() const noexcept { return magic == kMagicFlicker; }
    };

    /**
     * Parse the 128-byte file header from the start of a SEQ file.
     */
    [[nodiscard]] expected<file_header>
        parse_file_header(std::span<const std::uint8_t> data);

    // ------------------------------------------------------------------------
    // cel_header — 128-byte per-frame header (NEOchrome-style)
    // ------------------------------------------------------------------------

    enum class operation : std::uint8_t {
        copy = 0,
        xor_op = 1,
    };

    enum class storage : std::uint8_t {
        uncompressed = 0,
        word_rle     = 1,
    };

    struct cel_header {
        std::uint16_t type;          ///< must be 0xFFFF
        std::uint16_t resolution;    ///< 0=320x200x4-plane, 1=640x200x2, 2=640x400x1
        std::uint16_t palette[16];   ///< ST 0x0RGB packed words (3-bit-per-channel)
        // bytes 36..47: filename[12]    — skipped
        // bytes 48..55: color animation — skipped
        std::uint16_t x_offset;
        std::uint16_t y_offset;
        std::uint16_t width;
        std::uint16_t height;
        operation     op;
        storage       sm;
        std::uint32_t data_size;     ///< bytes of frame data following the header
        // bytes 68..127: 60 bytes reserved — skipped
    };

    /**
     * Parse the 128-byte cel header from the start of a frame chunk.
     * Validates type==0xFFFF and resolution<=2.
     */
    [[nodiscard]] expected<cel_header>
        parse_cel_header(std::span<const std::uint8_t> data);

    // ------------------------------------------------------------------------
    // Resolution-derived dimensions
    // ------------------------------------------------------------------------

    struct resolution_info {
        unsigned int width;
        unsigned int height;
        unsigned int planes;
        unsigned int colors;     // 1 << planes
    };

    [[nodiscard]] expected<resolution_info>
        info_for_resolution(std::uint16_t resolution);
} // namespace atari_seq
