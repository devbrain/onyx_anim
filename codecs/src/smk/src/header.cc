#include <smk/header.hh>

namespace smk {
    expected<file_header>
    parse_file_header(std::span <const std::uint8_t> data) {
        if (data.size() < kHeaderSize) {
            return make_unexpected<error_type>("smk: file smaller than header");
        }

        file_header h{};
        h.magic[0] = static_cast <char>(data[0]);
        h.magic[1] = static_cast <char>(data[1]);
        h.magic[2] = static_cast <char>(data[2]);
        h.magic[3] = static_cast <char>(data[3]);
        if (!(h.magic[0] == 'S' && h.magic[1] == 'M' && h.magic[2] == 'K' &&
              (h.magic[3] == '2' || h.magic[3] == '4'))) {
            return make_unexpected<error_type>("smk: missing SMK2/SMK4 magic");
        }
        h.is_smk4 = (h.magic[3] == '4');

        // Body is little-endian throughout. The file header has 7×u32
        // audio_size and 7×u32 audio_rate fields; we store them into the
        // 7-element `audio` array.
        byte_reader br{data.subspan(4)};
        std::uint32_t pts_inc_u = 0;
        std::array <std::uint32_t, 7> audio_size{};
        std::array <std::uint32_t, 7> audio_rate{};
        std::uint32_t dummy = 0;

        br >> h.width >> h.height >> h.frames >> pts_inc_u >> h.flags;
        for (auto& s : audio_size) br >> s;
        br >> h.trees_size >> h.mmap_size >> h.mclr_size
           >> h.full_size >> h.type_size;
        for (auto& r : audio_rate) br >> r;
        br >> dummy;
        if (!br) {
            return make_unexpected<error_type>("smk: header truncated mid-read");
        }

        h.pts_inc = static_cast <std::int32_t>(pts_inc_u);
        if (h.flags & kSmkRingFrame) {
            // The "ring frame" is a redundant copy of frame 0 appended
            // to the stream so player loops back-to-back without a
            // visible jump. ffmpeg counts it as a real frame; we do the
            // same so the frame_size table length is correct.
            ++h.frames;
        }

        if (h.frames == 0u) {
            return make_unexpected<error_type>("smk: zero frames");
        }
        if (h.width == 0u || h.height == 0u) {
            return make_unexpected<error_type>("smk: zero dimensions");
        }

        for (std::size_t i = 0; i < 7; ++i) {
            auto& a = h.audio[i];
            a.max_chunk_bytes = audio_size[i];
            a.sample_rate = audio_rate[i] & 0x00FFFFFFu;
            a.flags       = static_cast <std::uint8_t>(audio_rate[i] >> 24u);
            a.present     = a.sample_rate != 0u;
            a.stereo      = (a.flags & kSAudStereo) != 0;
            a.bits16      = (a.flags & kSAud16Bits) != 0;
            a.packed      = (a.flags & kSAudPacked) != 0;
            a.unsupported = (a.flags & (kSAudBinkAud | kSAudUseDct)) != 0;
        }

        return h;
    }

    std::int64_t frame_period_us(std::int32_t pts_inc) noexcept {
        if (pts_inc > 0) {
            // Stored value is in milliseconds.
            return static_cast <std::int64_t>(pts_inc) * 1000LL;
        }
        if (pts_inc < 0) {
            // ffmpeg sets AVStream time_base to 100000/(-pts_inc) with one
            // tick per frame, so frame period = (-pts_inc) / 100000
            // seconds = (-pts_inc) × 10 microseconds. Equivalently:
            // fps = 100000 / (-pts_inc).
            return -static_cast <std::int64_t>(pts_inc) * 10LL;
        }
        return 100'000LL; // 10 fps fallback
    }
} // namespace smk
