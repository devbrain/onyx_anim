#include <anim/header.hh>

#include <cstring>

namespace anim {
    expected<bmhd>
    parse_bmhd(std::span<const std::uint8_t> data) {
        if (data.size() < 20) {
            return make_unexpected("anim: BMHD truncated");
        }
        byte_reader br{data};
        bmhd h{};
        std::uint8_t mask_byte    = 0;
        std::uint8_t compress_byte = 0;
        br >> h.width >> h.height
           >> h.x_origin >> h.y_origin
           >> h.planes
           >> mask_byte
           >> compress_byte
           >> h.pad1
           >> h.transparent
           >> h.x_aspect >> h.y_aspect
           >> h.page_width >> h.page_height;
        h.mask     = static_cast<masking>(mask_byte);
        h.compress = static_cast<compression>(compress_byte);
        if (!br) return make_unexpected("anim: BMHD truncated mid-read");
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
        byte_reader br{data};
        anhd h{};
        br >> h.operation >> h.mask
           >> h.w >> h.h
           >> h.x >> h.y
           >> h.abstime >> h.reltime
           >> h.interleave >> h.pad0
           >> h.bits;
        if (!br) return make_unexpected("anim: ANHD truncated mid-read");
        if (data.size() >= 40) {
            std::memcpy(h.pad, data.data() + 24, 16);
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

    expected<sxhd>
    parse_sxhd(std::span<const std::uint8_t> data) {
        // Body must hold the 20 fixed bytes; the 2-byte Loop trailer is optional.
        if (data.size() < 20) {
            return make_unexpected("anim: SXHD truncated (need >= 20 bytes)");
        }
        byte_reader br{data};
        sxhd h{};
        br >> h.sample_depth
           >> h.fixed_volume
           >> h.length
           >> h.play_rate
           >> h.compression
           >> h.used_channels
           >> h.used_mode
           >> h.play_freq;
        if (data.size() >= 22u) {
            br >> h.loop;
        }
        if (!br) {
            return make_unexpected("anim: SXHD truncated mid-read");
        }
        return h;
    }

    expected<std::uint32_t>
    parse_camg(std::span<const std::uint8_t> data) {
        if (data.size() < 4) {
            return make_unexpected("anim: CAMG truncated");
        }
        byte_reader br{data};
        std::uint32_t v = 0;
        br >> v;
        if (!br) return make_unexpected("anim: CAMG truncated mid-read");
        return v;
    }
} // namespace anim
