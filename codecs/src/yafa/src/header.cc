#include <yafa/header.hh>

#include <vector>

namespace yafa {
    expected<info>
    parse_info(std::span<const std::uint8_t> data) {
        if (data.size() < 14) {
            return make_unexpected("yafa: INFO chunk truncated");
        }
        byte_reader br{data};
        info h{};
        std::uint16_t ftype_raw = 0;
        br >> h.width >> h.height >> h.depth >> h.speed >> h.frames
           >> ftype_raw >> h.flags;
        if (!br) return make_unexpected("yafa: INFO truncated mid-read");

        switch (ftype_raw) {
            case 0: h.type = frame_type::planar;     break;
            case 1: h.type = frame_type::planar_xpk; break;
            case 3: h.type = frame_type::chunky_xpk; break;
            case 4: h.type = frame_type::chunky8;    break;
            default:
                return make_unexpected("yafa: unsupported INFO frametype");
        }

        h.ham         = (h.flags & 0x0001u) != 0u;
        h.dyn_palette = (h.flags & 0x0002u) != 0u;
        h.delta       = (h.flags & 0x0004u) != 0u;
        if (h.delta) {
            // "BIT 4 set - LONG; else BIT 3 set - WORD; else BYTE"
            if      (h.flags & 0x0010u) h.delta_w = delta_kind::dlong;
            else if (h.flags & 0x0008u) h.delta_w = delta_kind::word;
            else                        h.delta_w = delta_kind::byte;
        } else {
            h.delta_w = delta_kind::none;
        }

        if (h.width  == 0u || h.width  > 8192u ||
            h.height == 0u || h.height > 8192u ||
            h.depth  == 0u || h.depth  > 8u) {
            return make_unexpected("yafa: INFO field out of range");
        }
        // Spec mandates width multiple of 16 (planar bitplane row alignment).
        if ((h.width & 0x0Fu) != 0u) {
            return make_unexpected("yafa: width must be a multiple of 16");
        }
        return h;
    }

    expected<std::vector<std::uint8_t>>
    parse_drgb(std::span<const std::uint8_t> data) {
        if (data.size() < 4u) {
            return make_unexpected("yafa: DRGB chunk truncated");
        }
        byte_reader br{data};
        std::uint16_t count = 0;
        std::uint16_t first = 0;
        br >> count >> first;
        if (!br || count == 0u) return std::vector<std::uint8_t>{};
        if (data.size() < 4u + static_cast<std::size_t>(count) * 12u) {
            return make_unexpected("yafa: DRGB body truncated");
        }
        std::vector<std::uint8_t> out;
        out.reserve(static_cast<std::size_t>(count) * 3u);
        for (std::uint16_t i = 0; i < count; ++i) {
            std::uint32_t r = 0, g = 0, b = 0;
            br >> r >> g >> b;
            out.push_back(static_cast<std::uint8_t>((r >> 24) & 0xFFu));
            out.push_back(static_cast<std::uint8_t>((g >> 24) & 0xFFu));
            out.push_back(static_cast<std::uint8_t>((b >> 24) & 0xFFu));
        }
        if (!br) return make_unexpected("yafa: DRGB truncated mid-read");
        return out;
    }

    expected<std::vector<std::uint32_t>>
    parse_prof(std::span<const std::uint8_t> data) {
        if ((data.size() & 0x3u) != 0u) {
            return make_unexpected("yafa: PROF size not a multiple of 4");
        }
        byte_reader br{data};
        std::vector<std::uint32_t> out;
        out.resize(data.size() / 4u);
        for (auto& v : out) br >> v;
        if (!br) return make_unexpected("yafa: PROF truncated mid-read");
        return out;
    }
} // namespace yafa
