#include <onyx_anim/sdk/convert.hh>

#include <vector>

namespace onyx_anim {
    namespace {
        // Pre-compute: read one row of indexed8 pixels through `palette` (RGB
        // triplets) and write the expanded RGB888 bytes into `out`. Caller
        // sized `out` to width*3.
        void expand_indexed_to_rgb888(const std::uint8_t* row_indices,
                                      const std::uint8_t* palette,
                                      unsigned int        width,
                                      std::uint8_t*       out) noexcept {
            for (unsigned int x = 0; x < width; ++x) {
                const std::size_t pi = static_cast <std::size_t>(row_indices[x]) * 3u;
                out[x * 3 + 0] = palette[pi + 0];
                out[x * 3 + 1] = palette[pi + 1];
                out[x * 3 + 2] = palette[pi + 2];
            }
        }

        void expand_indexed_to_rgba8888(const std::uint8_t* row_indices,
                                        const std::uint8_t* palette,
                                        unsigned int        width,
                                        std::uint8_t*       out) noexcept {
            for (unsigned int x = 0; x < width; ++x) {
                const std::size_t pi = static_cast <std::size_t>(row_indices[x]) * 3u;
                out[x * 4 + 0] = palette[pi + 0];
                out[x * 4 + 1] = palette[pi + 1];
                out[x * 4 + 2] = palette[pi + 2];
                out[x * 4 + 3] = 0xFFu;
            }
        }

        void expand_rgb888_to_rgba8888(const std::uint8_t* row_rgb,
                                       unsigned int        width,
                                       std::uint8_t*       out) noexcept {
            for (unsigned int x = 0; x < width; ++x) {
                out[x * 4 + 0] = row_rgb[x * 3 + 0];
                out[x * 4 + 1] = row_rgb[x * 3 + 1];
                out[x * 4 + 2] = row_rgb[x * 3 + 2];
                out[x * 4 + 3] = 0xFFu;
            }
        }
    } // namespace

    result convert_surface(const onyx_image::memory_surface& src,
                           onyx_image::surface&              dst,
                           pixel_format                       dst_format) {
        const int w = src.width();
        const int h = src.height();
        if (w <= 0 || h <= 0) {
            return make_unexpected<error_type>("convert_surface: zero-sized source");
        }
        if (!dst.set_size(w, h, dst_format)) {
            return make_unexpected<error_type>("convert_surface: dst.set_size rejected");
        }

        const auto src_format = src.format();
        const auto src_pixels = src.pixels();
        const auto src_pitch  = src.pitch();
        const auto src_palette = src.palette();
        const unsigned int uw = static_cast <unsigned int>(w);

        // Same-format pass-through. The most common fast path for engines
        // that match the codec's native format directly (e.g. paletted8
        // games handing the player an indexed8 surface).
        if (src_format == dst_format) {
            if (src_format == pixel_format::indexed8) {
                // Forward palette before pixels so the renderer can
                // resolve indices on the next write_pixels.
                if (!src_palette.empty()) {
                    const int palette_count =
                        static_cast <int>(src_palette.size() / 3u);
                    dst.set_palette_size(palette_count);
                    dst.write_palette(0, src_palette);
                }
            }
            const std::size_t bpp =
                onyx_image::bytes_per_pixel(src_format);
            const std::size_t row_bytes =
                static_cast <std::size_t>(uw) * bpp;
            for (int y = 0; y < h; ++y) {
                dst.write_pixels(0, y, static_cast <int>(row_bytes),
                                 src_pixels.data() +
                                     static_cast <std::size_t>(y) * src_pitch);
            }
            return {};
        }

        // Cross-format conversions: src layout dictates the path.
        if (src_format == pixel_format::indexed8) {
            if (src_palette.empty()) {
                return make_unexpected<error_type>(
                    "convert_surface: indexed8 source has no palette");
            }
            if (dst_format == pixel_format::rgb888) {
                std::vector <std::uint8_t> row(static_cast <std::size_t>(uw) * 3u);
                for (int y = 0; y < h; ++y) {
                    expand_indexed_to_rgb888(
                        src_pixels.data() +
                            static_cast <std::size_t>(y) * src_pitch,
                        src_palette.data(), uw, row.data());
                    dst.write_pixels(0, y, static_cast <int>(row.size()),
                                     row.data());
                }
                return {};
            }
            if (dst_format == pixel_format::rgba8888) {
                std::vector <std::uint8_t> row(static_cast <std::size_t>(uw) * 4u);
                for (int y = 0; y < h; ++y) {
                    expand_indexed_to_rgba8888(
                        src_pixels.data() +
                            static_cast <std::size_t>(y) * src_pitch,
                        src_palette.data(), uw, row.data());
                    dst.write_pixels(0, y, static_cast <int>(row.size()),
                                     row.data());
                }
                return {};
            }
        }
        if (src_format == pixel_format::rgb888) {
            if (dst_format == pixel_format::rgba8888) {
                std::vector <std::uint8_t> row(static_cast <std::size_t>(uw) * 4u);
                for (int y = 0; y < h; ++y) {
                    expand_rgb888_to_rgba8888(
                        src_pixels.data() +
                            static_cast <std::size_t>(y) * src_pitch,
                        uw, row.data());
                    dst.write_pixels(0, y, static_cast <int>(row.size()),
                                     row.data());
                }
                return {};
            }
        }

        // Unsupported direction (e.g. rgba8888 → indexed8 needs
        // colour-quantisation, rgba8888 → rgb888 needs a chosen alpha
        // policy). Engines wanting these can post-process themselves.
        return make_unexpected<error_type>(
            "convert_surface: unsupported format pair");
    }
} // namespace onyx_anim
