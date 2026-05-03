#pragma once

// Bink container header (per http://wiki.multimedia.cx/index.php?title=Bink_Container
// and ffmpeg's libavformat/bink.c). Layout is little-endian throughout.
//
//   +0   "BIK[bfghi k]"      (4 bytes)
//   +4   file_size − 8       (u32; total file = +8)
//   +8   num_frames          (u32)
//   +12  largest_frame_size  (u32)
//   +16  duration            (u32, redundant with num_frames)
//   +20  width               (u32)
//   +24  height              (u32)
//   +28  fps_num             (u32)
//   +32  fps_den             (u32)
//   +36  flags               (u32, "extradata" — bit 0x00100000 = alpha
//                             plane, bit 0x00020000 = grayscale)
//   +40  num_audio_tracks    (u32)
//   [+44 if magic suffix is 'k': skip extra u32]
//   for each audio track: u32 max_decoded_size
//   for each audio track: u16 sample_rate, u16 flags
//   for each audio track: u32 track_id
//   then num_frames+1 × u32 frame index entries
//     bit 0 of entry = keyframe flag, value & ~1 = absolute file offset
//   (last entry's "value" is past-end of all frame data)

#include <bink/types.hh>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bink {
    inline constexpr std::uint32_t kBinkFlagAlpha = 0x00100000u;
    inline constexpr std::uint32_t kBinkFlagGray  = 0x00020000u;

    enum aud_flag : std::uint16_t {
        kBinkAud16Bits = 0x4000, // 16-bit output preference
        kBinkAudStereo = 0x2000,
        kBinkAudUseDct = 0x1000, // BinkAudio DCT (else RDFT)
    };

    struct audio_track {
        std::uint32_t max_decoded_bytes = 0;
        std::uint16_t sample_rate       = 0;
        std::uint16_t flags             = 0;
        std::uint32_t track_id          = 0;
        bool          stereo            = false;
        bool          use_dct           = false;
        bool          want_16bit        = false;
    };

    struct frame_index_entry {
        std::uint64_t offset   = 0;   // absolute file offset
        std::uint32_t size     = 0;   // total chunk size in bytes
        bool          keyframe = false;
    };

    struct file_header {
        char        magic[4]    = {0, 0, 0, 0};
        char        version     = 0;        // magic[3]: 'b', 'f', 'g', 'h', 'i', 'k'
        std::uint32_t width     = 0;
        std::uint32_t height    = 0;
        std::uint32_t num_frames = 0;
        std::uint32_t fps_num   = 0;
        std::uint32_t fps_den   = 0;
        std::uint32_t flags     = 0;
        bool        has_alpha   = false;
        bool        is_gray     = false;
        std::vector <audio_track>       audio;
        std::vector <frame_index_entry> frames;
    };

    // Parse the entire header (fixed prefix + audio tracks + frame index).
    // `data` should be the full file (or at least enough to reach the
    // last frame_index entry); we read the index strictly bytewise.
    expected<file_header>
    parse_file_header(std::span <const std::uint8_t> data);

    // Frame period in microseconds: fps_den / fps_num seconds.
    [[nodiscard]] inline std::int64_t
    frame_period_us(std::uint32_t fps_num, std::uint32_t fps_den) noexcept {
        if (fps_num == 0) return 0;
        return static_cast <std::int64_t>(fps_den) * 1'000'000LL /
               static_cast <std::int64_t>(fps_num);
    }
} // namespace bink
