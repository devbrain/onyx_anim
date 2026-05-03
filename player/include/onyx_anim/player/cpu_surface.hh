#pragma once

#include <onyx_image/surface.hpp>
#include <onyx_image/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace onyx_anim {
    /**
     * A minimal CPU-backed `onyx_image::surface` implementation for engines
     * that don't have anything fancier (zero-copy streaming texture
     * mapping, GPU-side decoders, etc.) ready to plug in. Decoded frames
     * land in a contiguous byte buffer; engines upload them however their
     * renderer prefers.
     *
     * Header-only on purpose — it's a few dozen lines, every engine wants
     * a slightly different upload tail-end, and forcing a sub-library
     * link for it would just be friction.
     */
    class cpu_surface final : public onyx_image::surface {
        public:
            cpu_surface() = default;

            cpu_surface(const cpu_surface&) = delete;
            cpu_surface& operator=(const cpu_surface&) = delete;
            cpu_surface(cpu_surface&&) noexcept = default;
            cpu_surface& operator=(cpu_surface&&) noexcept = default;

            // ---- onyx_image::surface ----
            bool set_size(int w, int h, onyx_image::pixel_format f) override {
                if (w < 0 || h < 0) return false;
                width_  = w;
                height_ = h;
                format_ = f;
                bpp_    = onyx_image::bytes_per_pixel(f);
                pixels_.assign(static_cast<std::size_t>(w) *
                               static_cast<std::size_t>(h) * bpp_, 0);
                return true;
            }

            void write_pixels(int x, int y, int count,
                              const std::uint8_t* src) override {
                if (y < 0 || y >= height_ || x < 0 || count <= 0) return;
                const std::size_t row_off =
                    static_cast<std::size_t>(y) *
                    static_cast<std::size_t>(width_) * bpp_ +
                    static_cast<std::size_t>(x);
                if (row_off + static_cast<std::size_t>(count) > pixels_.size()) return;
                std::memcpy(pixels_.data() + row_off, src,
                            static_cast<std::size_t>(count));
            }

            void write_pixel(int x, int y, std::uint8_t v) override {
                if (x < 0 || x >= width_ || y < 0 || y >= height_) return;
                const std::size_t off =
                    static_cast<std::size_t>(y) *
                    static_cast<std::size_t>(width_) * bpp_ +
                    static_cast<std::size_t>(x) * bpp_;
                if (off >= pixels_.size()) return;
                pixels_[off] = v;
            }

            void set_palette_size(int count) override {
                if (count < 0) return;
                palette_.assign(static_cast<std::size_t>(count) * 3u, 0);
            }

            void write_palette(int start,
                               std::span<const std::uint8_t> colors) override {
                if (start < 0) return;
                const std::size_t off = static_cast<std::size_t>(start) * 3u;
                if (off >= palette_.size()) return;
                const std::size_t n =
                    std::min(colors.size(), palette_.size() - off);
                std::memcpy(palette_.data() + off, colors.data(), n);
            }

            // ---- accessors ----
            [[nodiscard]] int width()  const noexcept { return width_;  }
            [[nodiscard]] int height() const noexcept { return height_; }
            [[nodiscard]] onyx_image::pixel_format format() const noexcept { return format_; }
            [[nodiscard]] std::size_t pitch() const noexcept {
                return width_ > 0
                    ? static_cast<std::size_t>(width_) * bpp_
                    : 0u;
            }
            [[nodiscard]] const std::uint8_t* data() const noexcept { return pixels_.data(); }
            [[nodiscard]] std::size_t size() const noexcept { return pixels_.size(); }
            [[nodiscard]] const std::uint8_t* palette_data() const noexcept { return palette_.data(); }
            [[nodiscard]] std::size_t palette_size() const noexcept { return palette_.size(); }

        private:
            std::vector<std::uint8_t> pixels_;
            std::vector<std::uint8_t> palette_;
            int width_  = 0;
            int height_ = 0;
            onyx_image::pixel_format format_ = onyx_image::pixel_format::rgba8888;
            std::size_t bpp_ = 4;
    };
} // namespace onyx_anim
