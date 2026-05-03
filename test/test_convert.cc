#include <doctest/doctest.h>

#include <onyx_anim/sdk/convert.hh>

#include <onyx_image/surface.hpp>

#include <array>
#include <cstdint>
#include <vector>

using onyx_anim::convert_surface;
using onyx_anim::pixel_format;

namespace {

// Build a 2x2 indexed8 surface with a tiny palette: indices 0..3 → red,
// green, blue, white.
onyx_image::memory_surface make_indexed_2x2() {
    onyx_image::memory_surface s;
    s.set_size(2, 2, pixel_format::indexed8);
    s.set_palette_size(4);
    constexpr std::array<std::uint8_t, 12> pal = {
        0xFF, 0x00, 0x00,   // 0 red
        0x00, 0xFF, 0x00,   // 1 green
        0x00, 0x00, 0xFF,   // 2 blue
        0xFF, 0xFF, 0xFF,   // 3 white
    };
    s.write_palette(0, pal);
    constexpr std::array<std::uint8_t, 4> pixels = {0, 1, 2, 3};
    s.write_pixels(0, 0, 2, pixels.data());
    s.write_pixels(0, 1, 2, pixels.data() + 2);
    return s;
}

onyx_image::memory_surface make_rgb888_2x1() {
    onyx_image::memory_surface s;
    s.set_size(2, 1, pixel_format::rgb888);
    constexpr std::array<std::uint8_t, 6> pixels = {
        0x10, 0x20, 0x30,
        0xA0, 0xB0, 0xC0,
    };
    s.write_pixels(0, 0, 6, pixels.data());
    return s;
}

} // namespace

TEST_CASE("convert_surface: indexed8 → rgba8888 expands palette + opaque alpha") {
    auto src = make_indexed_2x2();
    onyx_image::memory_surface dst;
    REQUIRE(convert_surface(src, dst, pixel_format::rgba8888));
    REQUIRE(dst.format() == pixel_format::rgba8888);
    REQUIRE(dst.width()  == 2);
    REQUIRE(dst.height() == 2);

    const auto px = dst.pixels();
    REQUIRE(px.size() >= 16);  // 2x2 * 4 bytes
    // Row 0: [red, green]
    CHECK(px[0]  == 0xFF); CHECK(px[1]  == 0x00); CHECK(px[2]  == 0x00); CHECK(px[3]  == 0xFF);
    CHECK(px[4]  == 0x00); CHECK(px[5]  == 0xFF); CHECK(px[6]  == 0x00); CHECK(px[7]  == 0xFF);
    // Row 1: [blue, white]
    const auto pitch = dst.pitch();
    const auto* row1 = px.data() + pitch;
    CHECK(row1[0] == 0x00); CHECK(row1[1] == 0x00); CHECK(row1[2] == 0xFF); CHECK(row1[3] == 0xFF);
    CHECK(row1[4] == 0xFF); CHECK(row1[5] == 0xFF); CHECK(row1[6] == 0xFF); CHECK(row1[7] == 0xFF);
}

TEST_CASE("convert_surface: indexed8 → rgb888 expands palette without alpha") {
    auto src = make_indexed_2x2();
    onyx_image::memory_surface dst;
    REQUIRE(convert_surface(src, dst, pixel_format::rgb888));
    REQUIRE(dst.format() == pixel_format::rgb888);

    const auto px = dst.pixels();
    REQUIRE(px.size() >= 12);
    CHECK(px[0] == 0xFF); CHECK(px[1] == 0x00); CHECK(px[2] == 0x00);  // red
    CHECK(px[3] == 0x00); CHECK(px[4] == 0xFF); CHECK(px[5] == 0x00);  // green
}

TEST_CASE("convert_surface: rgb888 → rgba8888 adds opaque alpha") {
    auto src = make_rgb888_2x1();
    onyx_image::memory_surface dst;
    REQUIRE(convert_surface(src, dst, pixel_format::rgba8888));
    REQUIRE(dst.format() == pixel_format::rgba8888);

    const auto px = dst.pixels();
    REQUIRE(px.size() >= 8);
    CHECK(px[0] == 0x10); CHECK(px[1] == 0x20); CHECK(px[2] == 0x30); CHECK(px[3] == 0xFF);
    CHECK(px[4] == 0xA0); CHECK(px[5] == 0xB0); CHECK(px[6] == 0xC0); CHECK(px[7] == 0xFF);
}

TEST_CASE("convert_surface: same-format pass-through preserves palette") {
    auto src = make_indexed_2x2();
    onyx_image::memory_surface dst;
    REQUIRE(convert_surface(src, dst, pixel_format::indexed8));
    REQUIRE(dst.format() == pixel_format::indexed8);
    REQUIRE(dst.width()  == 2);

    // Palette propagated.
    const auto pal = dst.palette();
    REQUIRE(pal.size() >= 12);
    CHECK(pal[0] == 0xFF); CHECK(pal[1] == 0x00); CHECK(pal[2] == 0x00);
    CHECK(pal[9] == 0xFF); CHECK(pal[10] == 0xFF); CHECK(pal[11] == 0xFF);

    const auto px = dst.pixels();
    REQUIRE(px.size() >= 4);
    CHECK(px[0] == 0); CHECK(px[1] == 1);
}

TEST_CASE("convert_surface: rgba8888 → indexed8 is rejected (would need quantisation)") {
    onyx_image::memory_surface src;
    src.set_size(2, 1, pixel_format::rgba8888);
    constexpr std::array<std::uint8_t, 8> pixels = {
        0x10, 0x20, 0x30, 0xFF,
        0xA0, 0xB0, 0xC0, 0xFF,
    };
    src.write_pixels(0, 0, 8, pixels.data());

    onyx_image::memory_surface dst;
    auto r = convert_surface(src, dst, pixel_format::indexed8);
    CHECK_FALSE(r);
}
