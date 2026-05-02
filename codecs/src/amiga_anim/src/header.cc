#include <anim/header.hh>

#include <cstring>

namespace anim {
    namespace {
        std::uint16_t r16(const std::uint8_t* p) noexcept {
            return static_cast<std::uint16_t>(
                (static_cast<unsigned>(p[0]) << 8u) | p[1]);
        }
        std::int16_t  rs16(const std::uint8_t* p) noexcept {
            return static_cast<std::int16_t>(r16(p));
        }
        std::uint32_t r32(const std::uint8_t* p) noexcept {
            return (static_cast<std::uint32_t>(p[0]) << 24u) |
                   (static_cast<std::uint32_t>(p[1]) << 16u) |
                   (static_cast<std::uint32_t>(p[2]) <<  8u) |
                    static_cast<std::uint32_t>(p[3]);
        }
    } // namespace

    expected<bmhd>
    parse_bmhd(std::span<const std::uint8_t> data) {
        if (data.size() < 20) {
            return make_unexpected("anim: BMHD truncated");
        }
        const auto* p = data.data();
        bmhd h{};
        h.width        = r16(p + 0);
        h.height       = r16(p + 2);
        h.x_origin     = rs16(p + 4);
        h.y_origin     = rs16(p + 6);
        h.planes       = p[8];
        h.mask         = static_cast<masking>(p[9]);
        h.compress     = static_cast<compression>(p[10]);
        h.pad1         = p[11];
        h.transparent  = r16(p + 12);
        h.x_aspect     = p[14];
        h.y_aspect     = p[15];
        h.page_width   = rs16(p + 16);
        h.page_height  = rs16(p + 18);
        return h;
    }

    expected<anhd>
    parse_anhd(std::span<const std::uint8_t> data) {
        if (data.size() < 24) {
            // pad[16] is at the end; some files have only the leading 24 bytes
            // populated and then pad/extension. We require at least the header
            // up to `bits` (24 bytes) — pad[16] is optional.
            return make_unexpected("anim: ANHD truncated");
        }
        const auto* p = data.data();
        anhd h{};
        h.operation  = p[0];
        h.mask       = p[1];
        h.w          = r16(p + 2);
        h.h          = r16(p + 4);
        h.x          = rs16(p + 6);
        h.y          = rs16(p + 8);
        h.abstime    = r32(p + 10);
        h.reltime    = r32(p + 14);
        h.interleave = p[18];
        h.pad0       = p[19];
        h.bits       = r32(p + 20);
        if (data.size() >= 40) {
            std::memcpy(h.pad, p + 24, 16);
        }
        return h;
    }

    expected<std::vector<std::uint8_t>>
    parse_cmap(std::span<const std::uint8_t> data) {
        const std::size_t n = data.size() / 3u;
        if (n > 256) {
            return make_unexpected("anim: CMAP has more than 256 entries");
        }
        std::vector<std::uint8_t> out(n * 3u);
        std::memcpy(out.data(), data.data(), out.size());
        return out;
    }

    expected<std::uint32_t>
    parse_camg(std::span<const std::uint8_t> data) {
        if (data.size() < 4) {
            return make_unexpected("anim: CAMG truncated");
        }
        return r32(data.data());
    }
} // namespace anim
