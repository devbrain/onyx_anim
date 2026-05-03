#include <doctest/doctest.h>

#include <onyx_anim/player/cpu_surface.hh>

using onyx_anim::cpu_surface;
using onyx_image::pixel_format;

TEST_CASE("cpu_surface: set_size allocates the right buffer for each format") {
    cpu_surface s;

    REQUIRE(s.set_size(8, 4, pixel_format::rgba8888));
    CHECK(s.width()  == 8);
    CHECK(s.height() == 4);
    CHECK(s.pitch()  == 8u * 4u);
    CHECK(s.size()   == 8u * 4u * 4u);

    REQUIRE(s.set_size(8, 4, pixel_format::rgb888));
    CHECK(s.pitch() == 8u * 3u);
    CHECK(s.size()  == 8u * 4u * 3u);

    REQUIRE(s.set_size(8, 4, pixel_format::indexed8));
    CHECK(s.pitch() == 8u);
    CHECK(s.size()  == 8u * 4u);
}

TEST_CASE("cpu_surface: write_pixels lands at the byte offset within the row") {
    cpu_surface s;
    s.set_size(4, 2, pixel_format::rgba8888);

    const std::uint8_t row[16] = {
        1, 2, 3, 4,   5, 6, 7, 8,   9,10,11,12,  13,14,15,16,
    };
    s.write_pixels(0, 0, 16, row);
    s.write_pixels(0, 1, 16, row);

    const auto* p = s.data();
    CHECK(p[0]  == 1);
    CHECK(p[15] == 16);
    CHECK(p[16] == 1);   // start of row 1
    CHECK(p[31] == 16);
}

TEST_CASE("cpu_surface: write_pixels rejects out-of-bounds writes") {
    cpu_surface s;
    s.set_size(4, 2, pixel_format::rgba8888);

    const std::uint8_t junk[16] = {0xFF};
    s.write_pixels(-1, 0, 16, junk);   // negative x
    s.write_pixels(0, -1, 16, junk);   // negative y
    s.write_pixels(0, 99, 16, junk);   // y past end
    s.write_pixels(0, 0, 999, junk);   // count overflows row

    // None of the above should have touched memory: still all zero.
    bool all_zero = true;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s.data()[i] != 0) { all_zero = false; break; }
    }
    CHECK(all_zero);
}

TEST_CASE("cpu_surface: write_pixel writes a single byte for indexed8") {
    cpu_surface s;
    s.set_size(4, 4, pixel_format::indexed8);

    s.write_pixel(2, 1, 0x42);
    CHECK(s.data()[1 * 4 + 2] == 0x42);

    s.write_pixel(-1, 0, 0xFF);    // out of bounds
    s.write_pixel(0, 99, 0xFF);
    // No corruption past the one valid write.
    CHECK(s.data()[0] == 0);
}

TEST_CASE("cpu_surface: palette write_palette stores RGB triplets") {
    cpu_surface s;
    s.set_size(4, 4, pixel_format::indexed8);
    s.set_palette_size(3);

    const std::uint8_t triplets[6] = { 10, 20, 30,   40, 50, 60 };
    s.write_palette(1, std::span<const std::uint8_t>(triplets, 6));

    CHECK(s.palette_size() == 9u); // 3 entries × 3 bytes
    CHECK(s.palette_data()[0] == 0);   // index 0 untouched
    CHECK(s.palette_data()[3] == 10);
    CHECK(s.palette_data()[4] == 20);
    CHECK(s.palette_data()[5] == 30);
    CHECK(s.palette_data()[6] == 40);
}
