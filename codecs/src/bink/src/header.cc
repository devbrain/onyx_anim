#include <bink/header.hh>

namespace bink {
    expected<file_header>
    parse_file_header(std::span <const std::uint8_t> data) {
        if (data.size() < 44) {
            return make_unexpected<error_type>("bink: file smaller than fixed header");
        }
        file_header h{};
        h.magic[0] = static_cast <char>(data[0]);
        h.magic[1] = static_cast <char>(data[1]);
        h.magic[2] = static_cast <char>(data[2]);
        h.magic[3] = static_cast <char>(data[3]);
        if (!(h.magic[0] == 'B' && h.magic[1] == 'I' && h.magic[2] == 'K' &&
              (h.magic[3] == 'b' || h.magic[3] == 'f' || h.magic[3] == 'g' ||
               h.magic[3] == 'h' || h.magic[3] == 'i' || h.magic[3] == 'k'))) {
            return make_unexpected<error_type>("bink: missing BIK[bfghik] magic");
        }
        h.version = h.magic[3];

        byte_reader br{data.subspan(4)};
        std::uint32_t file_size_minus_8 = 0;
        std::uint32_t largest = 0;
        std::uint32_t duration = 0;
        std::uint32_t num_audio = 0;
        br >> file_size_minus_8 >> h.num_frames >> largest >> duration
           >> h.width >> h.height >> h.fps_num >> h.fps_den
           >> h.flags >> num_audio;
        if (!br) {
            return make_unexpected<error_type>("bink: header truncated mid-read");
        }
        h.has_alpha = (h.flags & kBinkFlagAlpha) != 0;
        h.is_gray   = (h.flags & kBinkFlagGray)  != 0;

        if (h.num_frames == 0 || h.num_frames > 1'000'000u) {
            return make_unexpected<error_type>("bink: implausible frame count");
        }
        if (h.width == 0 || h.height == 0) {
            return make_unexpected<error_type>("bink: zero dimensions");
        }
        if (h.fps_num == 0 || h.fps_den == 0) {
            return make_unexpected<error_type>("bink: zero fps");
        }

        // Cursor inside the full-file buffer. We've consumed 44 bytes so far
        // (magic + 10 × u32).
        std::size_t pos = 44;

        // The 'k' revision adds an extra u32 here (per ffmpeg demuxer:
        // signature == 'BIK' && revision == 'k' → skip 4).
        if (h.version == 'k') {
            if (pos + 4 > data.size()) {
                return make_unexpected<error_type>("bink: BIK 'k' tail truncated");
            }
            pos += 4;
        }

        if (num_audio > 256) {
            return make_unexpected<error_type>("bink: implausible audio track count");
        }
        h.audio.assign(num_audio, audio_track{});

        // u32 max_decoded_size × num_audio
        if (num_audio > 0) {
            const std::size_t need = num_audio * 4u;
            if (pos + need > data.size()) {
                return make_unexpected<error_type>("bink: audio max-size table truncated");
            }
            for (std::uint32_t i = 0; i < num_audio; ++i) {
                const std::uint8_t* p = data.data() + pos + i * 4u;
                h.audio[i].max_decoded_bytes =
                    static_cast <std::uint32_t>(p[0]) |
                    (static_cast <std::uint32_t>(p[1]) << 8u) |
                    (static_cast <std::uint32_t>(p[2]) << 16u) |
                    (static_cast <std::uint32_t>(p[3]) << 24u);
            }
            pos += need;

            // Per-track u16 sample_rate + u16 flags (4 bytes each).
            const std::size_t need2 = num_audio * 4u;
            if (pos + need2 > data.size()) {
                return make_unexpected<error_type>("bink: audio header table truncated");
            }
            for (std::uint32_t i = 0; i < num_audio; ++i) {
                const std::uint8_t* p = data.data() + pos + i * 4u;
                h.audio[i].sample_rate =
                    static_cast <std::uint16_t>(p[0] | (p[1] << 8));
                h.audio[i].flags =
                    static_cast <std::uint16_t>(p[2] | (p[3] << 8));
                h.audio[i].stereo     = (h.audio[i].flags & kBinkAudStereo) != 0;
                h.audio[i].use_dct    = (h.audio[i].flags & kBinkAudUseDct) != 0;
                h.audio[i].want_16bit = (h.audio[i].flags & kBinkAud16Bits) != 0;
            }
            pos += need2;

            // Per-track u32 track_id.
            const std::size_t need3 = num_audio * 4u;
            if (pos + need3 > data.size()) {
                return make_unexpected<error_type>("bink: audio track-id table truncated");
            }
            for (std::uint32_t i = 0; i < num_audio; ++i) {
                const std::uint8_t* p = data.data() + pos + i * 4u;
                h.audio[i].track_id =
                    static_cast <std::uint32_t>(p[0]) |
                    (static_cast <std::uint32_t>(p[1]) << 8u) |
                    (static_cast <std::uint32_t>(p[2]) << 16u) |
                    (static_cast <std::uint32_t>(p[3]) << 24u);
            }
            pos += need3;
        }

        // Frame index: num_frames+1 × u32, last is past-end. Each entry's
        // bit 0 = keyframe flag, value & ~1 = absolute offset.
        const std::size_t need_idx = (h.num_frames + 1u) * 4u;
        if (pos + need_idx > data.size()) {
            return make_unexpected<error_type>("bink: frame index table truncated");
        }
        h.frames.reserve(h.num_frames);
        std::uint32_t cur_word =
            static_cast <std::uint32_t>(data[pos]) |
            (static_cast <std::uint32_t>(data[pos + 1]) << 8u) |
            (static_cast <std::uint32_t>(data[pos + 2]) << 16u) |
            (static_cast <std::uint32_t>(data[pos + 3]) << 24u);
        pos += 4;
        for (std::uint32_t i = 0; i < h.num_frames; ++i) {
            const std::uint8_t* p = data.data() + pos;
            const std::uint32_t next_word =
                static_cast <std::uint32_t>(p[0]) |
                (static_cast <std::uint32_t>(p[1]) << 8u) |
                (static_cast <std::uint32_t>(p[2]) << 16u) |
                (static_cast <std::uint32_t>(p[3]) << 24u);
            pos += 4;
            const std::uint32_t off = cur_word & ~1u;
            const std::uint32_t end = next_word & ~1u;
            const bool key = (cur_word & 1u) != 0u;
            if (end <= off) {
                return make_unexpected<error_type>("bink: frame index non-monotonic");
            }
            frame_index_entry e{};
            e.offset   = off;
            e.size     = end - off;
            e.keyframe = key;
            h.frames.push_back(e);
            cur_word = next_word;
        }
        return h;
    }
} // namespace bink
