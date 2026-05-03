#include <cdxl/header.hh>

namespace cdxl {
    expected<chunk_header>
    parse_chunk_header(std::span<const std::uint8_t> data) {
        if (data.size() < kChunkHeaderSize) {
            return make_unexpected("cdxl: chunk header truncated");
        }
        byte_reader br{data};

        std::uint8_t  type_byte = 0;
        std::uint8_t  info_byte = 0;
        std::uint8_t  reserved18 = 0;
        chunk_header h{};
        br >> type_byte
           >> info_byte
           >> h.csize_cur
           >> h.csize_prev
           >> h.fnumber
           >> h.width
           >> h.height
           >> reserved18
           >> h.planes
           >> h.cmap_bytes
           >> h.audio_bytes;
        if (!br) return make_unexpected("cdxl: chunk header truncated mid-read");

        // Validate `type` (one of CUSTOM/STANDARD/SPECIAL).
        switch (type_byte) {
            case 0x00: h.type = kind::custom;   break;
            case 0x01: h.type = kind::standard; break;
            case 0x02: h.type = kind::special;  break;
            default:
                return make_unexpected("cdxl: unknown chunk type");
        }

        // Validate video encoding (low 2 bits) and pixel orientation
        // (top 3 bits). `stereo` lives in bit 4.
        const auto venc = static_cast<std::uint8_t>(info_byte & 0x03u);
        switch (venc) {
            case 0x00: h.venc = video_encoding::rgb;      break;
            case 0x01: h.venc = video_encoding::ham;      break;
            case 0x02: h.venc = video_encoding::yuv;      break;
            case 0x03: h.venc = video_encoding::avm_dctv; break;
            default: __builtin_unreachable();  // 2 bits → exhausted above
        }

        const auto por = static_cast<std::uint8_t>(info_byte & 0xE0u);
        switch (por) {
            case 0x00: h.por = pixel_orientation::bit_planar;  break;
            case 0x20: h.por = pixel_orientation::byte_planar; break;
            case 0x40: h.por = pixel_orientation::chunky;      break;
            case 0x80: h.por = pixel_orientation::bit_line;    break;
            case 0xC0: h.por = pixel_orientation::byte_line;   break;
            default:
                return make_unexpected("cdxl: invalid pixel orientation");
        }

        h.stereo = (info_byte & 0x10u) != 0u;
        return h;
    }
} // namespace cdxl
