#pragma once

// Smacker file-header parser.
//
// On-disk layout (everything LE):
//   +0   "SMK2" or "SMK4"               (4)
//   +4   width                          (u32)
//   +8   height                         (u32)
//   +12  frames                         (u32)
//   +16  pts_inc                        (s32)
//             > 0 : milliseconds per frame
//             < 0 : (-pts_inc) × 10 microseconds per frame
//                   (i.e. AVStream time_base = 100000/(-pts_inc), one
//                   frame per tick — see ffmpeg's smacker_read_header)
//             = 0 : 10 fps fallback per spec, but seen in practice as
//                   "use ffmpeg's default" — we treat 0 as "unknown,
//                   fall back to 100 ms / 10 fps".
//   +20  flags                          (u32)  bit0=ring, bit1=Y_INTERLACE,
//                                              bit2=Y_DOUBLE
//   +24  audio_size[7]                  (7 × u32)
//   +52  trees_size                     (u32)  size of packed trees blob
//   +56  mmap_size                      (u32)
//   +60  mclr_size                      (u32)
//   +64  full_size                      (u32)
//   +68  type_size                      (u32)
//   +72  audio_rate[7]                  (7 × u32)  low 24 bits = sample rate
//                                                  top 8 bits = SAudFlags
//                                                  (PACKED|16BITS|STEREO|
//                                                   BINKAUD|USEDCT)
//   +100 dummy/padding                  (u32)
//   +104 trees                          (`trees_size` bytes)
//   +X   frame_sizes[frames]            (u32 each; bottom 2 bits flags)
//   +Y   frame_flags[frames]            (u8 each;  bit0=palette change,
//                                                  bits1..7=audio mask)
//   +Z   frame data, packed back-to-back
//
// `frames` is bumped by one when bit 0 of `flags` is set ("ring frame").

#include <smk/types.hh>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace smk {
    inline constexpr std::size_t kHeaderSize = 104;

    enum saud_flags : std::uint8_t {
        kSAudPacked  = 0x80,
        kSAud16Bits  = 0x20,
        kSAudStereo  = 0x10,
        kSAudBinkAud = 0x08,
        kSAudUseDct  = 0x04,
    };

    enum smk_flags : std::uint32_t {
        kSmkRingFrame   = 0x01,
        kSmkYInterlace  = 0x02,
        kSmkYDouble     = 0x04,
    };

    enum smk_frame_flag : std::uint8_t {
        kFrameHasPalette = 0x01, // bit 0 of frame_flags[]
        // bits 1..7 mark per-track audio chunks present
    };

    enum smk_size_flag : std::uint32_t {
        kSizeKeyframe = 0x01, // bit 0 of frame_size table entry
        // Actual size = entry & ~3
    };

    struct audio_track_info {
        std::uint32_t sample_rate = 0; // Hz; 0 = absent
        std::uint8_t  flags       = 0; // SAudFlags
        std::uint32_t max_chunk_bytes = 0;
        // Convenience derived fields:
        bool          present     = false;
        bool          stereo      = false;
        bool          bits16      = false;
        bool          packed      = false; // Smacker DPCM
        bool          unsupported = false; // BINKAUD or USEDCT
    };

    struct file_header {
        char        magic[4]       = {0, 0, 0, 0};
        bool        is_smk4        = false;
        std::uint32_t width        = 0;
        std::uint32_t height       = 0;
        std::uint32_t frames       = 0;     // post-ring-frame adjustment
        std::int32_t  pts_inc      = 0;
        std::uint32_t flags        = 0;
        std::uint32_t trees_size   = 0;
        std::uint32_t mmap_size    = 0;
        std::uint32_t mclr_size    = 0;
        std::uint32_t full_size    = 0;
        std::uint32_t type_size    = 0;
        std::array <audio_track_info, 7> audio{};
    };

    // Parse the 104-byte header. `data` must span at least kHeaderSize
    // bytes — caller is expected to have read the full file or at least
    // the prefix.
    expected<file_header>
    parse_file_header(std::span <const std::uint8_t> data);

    // Frame timing in microseconds per frame. Handles all three pts_inc
    // signs per the layout note above.
    [[nodiscard]] std::int64_t frame_period_us(std::int32_t pts_inc) noexcept;
} // namespace smk
