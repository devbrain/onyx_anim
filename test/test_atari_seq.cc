#include <doctest/doctest.h>

#include <atari_seq/decoders.hh>

#include <array>
#include <cstdint>

TEST_CASE("atari_seq: frame rect outside framebuffer is rejected") {
    std::array<std::uint8_t, 2> fb{};
    constexpr std::array<std::uint8_t, 4> data = {};

    auto r = atari_seq::apply_frame(
        data,
        fb.data(),
        2,                         // scanline_stride for a 16-pixel row
        2,                         // one 1-row bitplane
        1,
        16,
        0, 1,                      // y_offset past the only row
        16, 1,
        atari_seq::operation::copy,
        atari_seq::storage::word_rle);

    CHECK_FALSE(r);
}
