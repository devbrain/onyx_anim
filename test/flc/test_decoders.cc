#include <doctest/doctest.h>

#include <flc/decoders.hh>

#include <array>
#include <cstdint>

TEST_CASE("decode_black zeroes the framebuffer respecting pitch") {
    constexpr unsigned int W = 4;
    constexpr unsigned int H = 3;
    constexpr std::size_t pitch = 5;     // intentional > W to test pitch handling
    std::array<std::uint8_t, pitch * H> fb{};
    fb.fill(0xAB);

    flc::decode_black(fb.data(), pitch, W, H);

    for (unsigned int y = 0; y < H; ++y) {
        for (unsigned int x = 0; x < W; ++x) {
            CHECK(fb[y * pitch + x] == 0);
        }
        // pitch padding byte should be untouched
        CHECK(fb[y * pitch + W] == 0xAB);
    }
}

TEST_CASE("decode_copy writes raw pixels respecting pitch") {
    constexpr unsigned int W = 3;
    constexpr unsigned int H = 2;
    constexpr std::size_t pitch = 4;
    std::array<std::uint8_t, pitch * H> fb{};

    constexpr std::array<std::uint8_t, W * H> src = {
        1, 2, 3,
        4, 5, 6,
    };

    auto r = flc::decode_copy(src, fb.data(), pitch, W, H);
    REQUIRE(r);
    CHECK(fb[0 * pitch + 0] == 1);
    CHECK(fb[0 * pitch + 2] == 3);
    CHECK(fb[1 * pitch + 0] == 4);
    CHECK(fb[1 * pitch + 2] == 6);
}

TEST_CASE("decode_copy detects truncation") {
    std::array<std::uint8_t, 16> fb{};
    std::array<std::uint8_t, 3> src{};
    auto r = flc::decode_copy(src, fb.data(), 4, 4, 4);
    CHECK_FALSE(r);
}

TEST_CASE("decode_brun replicate run fills a single-line frame") {
    constexpr unsigned int W = 4;
    constexpr unsigned int H = 1;
    std::array<std::uint8_t, W> fb{};

    // Encoded line: [opcount, count=+4 (replicate), value=0xAA]
    constexpr std::array<std::uint8_t, 3> chunk = {0x01, 0x04, 0xAA};

    auto r = flc::decode_brun(chunk, fb.data(), W, W, H);
    REQUIRE(r);
    CHECK(fb[0] == 0xAA);
    CHECK(fb[1] == 0xAA);
    CHECK(fb[2] == 0xAA);
    CHECK(fb[3] == 0xAA);
}

TEST_CASE("decode_brun literal and replicate runs combine to fill a row") {
    constexpr unsigned int W = 6;
    constexpr unsigned int H = 1;
    std::array<std::uint8_t, W> fb{};

    // Encoded line:
    //   opcount  (ignored)
    //   count = -4 (literal): 0x01 0x02 0x03 0x04
    //   count = +2 (replicate): 0xFF
    constexpr std::array<std::uint8_t, 8> chunk = {
        0x02,                       // opcount (ignored)
        0xFC,                       // -4 → literal of 4 bytes
        0x01, 0x02, 0x03, 0x04,
        0x02,                       // +2 → replicate next byte twice
        0xFF,
    };

    auto r = flc::decode_brun(chunk, fb.data(), W, W, H);
    REQUIRE(r);
    CHECK(fb[0] == 0x01);
    CHECK(fb[1] == 0x02);
    CHECK(fb[2] == 0x03);
    CHECK(fb[3] == 0x04);
    CHECK(fb[4] == 0xFF);
    CHECK(fb[5] == 0xFF);
}

TEST_CASE("decode_brun handles multiple lines respecting pitch") {
    constexpr unsigned int W = 3;
    constexpr unsigned int H = 2;
    constexpr std::size_t pitch = 5;       // intentional > W
    std::array<std::uint8_t, pitch * H> fb{};
    fb.fill(0xAB);

    // Two lines, each "replicate three of a single value".
    constexpr std::array<std::uint8_t, 6> chunk = {
        0x01, 0x03, 0x10,    // line 0: replicate 0x10 three times
        0x01, 0x03, 0x20,    // line 1: replicate 0x20 three times
    };

    auto r = flc::decode_brun(chunk, fb.data(), pitch, W, H);
    REQUIRE(r);
    for (unsigned int x = 0; x < W; ++x) {
        CHECK(fb[0 * pitch + x] == 0x10);
        CHECK(fb[1 * pitch + x] == 0x20);
    }
    // Pitch padding bytes untouched on each row.
    CHECK(fb[0 * pitch + W] == 0xAB);
    CHECK(fb[1 * pitch + W] == 0xAB);
}

TEST_CASE("decode_brun detects truncated input") {
    std::array<std::uint8_t, 16> fb{};
    // A line claiming a 4-byte literal but only 2 bytes follow the count.
    constexpr std::array<std::uint8_t, 4> truncated = {0x01, 0xFC, 0x01, 0x02};
    auto r = flc::decode_brun(truncated, fb.data(), 4, 4, 1);
    CHECK_FALSE(r);
}

TEST_CASE("decode_brun rejects runs that overflow the row") {
    std::array<std::uint8_t, 16> fb{};
    // count=+10 (replicate 10 times) but row width is only 4
    constexpr std::array<std::uint8_t, 3> bad = {0x01, 0x0A, 0xCC};
    auto r = flc::decode_brun(bad, fb.data(), 4, 4, 1);
    CHECK_FALSE(r);
}

TEST_CASE("decode_lc applies deltas at the right rows") {
    constexpr unsigned int W = 6, H = 4;
    std::array<std::uint8_t, W * H> fb{};
    fb.fill(0xCC);  // delta base — should remain in untouched cells

    // Skip 1 line, then change 2 lines:
    //   line 1: 1 packet — col_skip=2, rle=+3 (literal AA BB CC)
    //   line 2: 1 packet — col_skip=0, rle=-4 (replicate 0x77 four times)
    constexpr std::array<std::uint8_t, 14> chunk = {
        0x01, 0x00,                       // line_skip = 1
        0x02, 0x00,                       // line_count = 2
        // line 1
        0x01,                             // packet_count = 1
        0x02, 0x03, 0xAA, 0xBB, 0xCC,
        // line 2
        0x01,
        0x00, 0xFC, 0x77,                 // 0xFC = -4 → replicate
    };

    auto r = flc::decode_lc(chunk, fb.data(), W, W, H);
    REQUIRE(r);

    // Row 0: untouched
    CHECK(fb[0 * W + 0] == 0xCC);
    // Row 1: cols 0,1 untouched; cols 2,3,4 = AA,BB,CC; col 5 untouched
    CHECK(fb[1 * W + 0] == 0xCC);
    CHECK(fb[1 * W + 1] == 0xCC);
    CHECK(fb[1 * W + 2] == 0xAA);
    CHECK(fb[1 * W + 3] == 0xBB);
    CHECK(fb[1 * W + 4] == 0xCC);  // could be 0xCC base or 0xCC literal — same value
    CHECK(fb[1 * W + 5] == 0xCC);  // untouched
    // Row 2: cols 0..3 = 0x77; cols 4,5 untouched
    for (unsigned int x = 0; x < 4; ++x) CHECK(fb[2 * W + x] == 0x77);
    CHECK(fb[2 * W + 4] == 0xCC);
    CHECK(fb[2 * W + 5] == 0xCC);
    // Row 3: untouched
    CHECK(fb[3 * W + 0] == 0xCC);
}

TEST_CASE("decode_lc honors skip-continuation (rle=0) for skip > 255") {
    constexpr unsigned int W = 300, H = 1;
    std::vector<std::uint8_t> fb(W, 0x33);

    // line_skip=0, line_count=1
    // packet 1: col_skip=255, rle=0 (no data)
    // packet 2: col_skip=44, rle=+1 (literal one byte 0x99)
    // → effective x = 255 + 44 = 299; write fb[299] = 0x99
    std::vector<std::uint8_t> chunk = {
        0x00, 0x00,
        0x01, 0x00,
        0x02,                              // packet_count
        0xFF, 0x00,                        // skip=255, rle=0
        0x2C, 0x01, 0x99,                  // skip=44, rle=+1, value=0x99
    };

    auto r = flc::decode_lc(std::span<const std::uint8_t>(chunk), fb.data(), W, W, H);
    REQUIRE(r);
    CHECK(fb[298] == 0x33);
    CHECK(fb[299] == 0x99);
}

TEST_CASE("decode_lc rejects line range past height") {
    std::array<std::uint8_t, 16> fb{};
    constexpr std::array<std::uint8_t, 4> chunk = {
        0x05, 0x00,   // line_skip = 5
        0x02, 0x00,   // line_count = 2
    };
    auto r = flc::decode_lc(chunk, fb.data(), 4, 4, 4);
    CHECK_FALSE(r);
}

TEST_CASE("decode_lc rejects column overflow") {
    std::array<std::uint8_t, 16> fb{};
    // col_skip=0, rle=+10 on a row of width 4
    constexpr std::array<std::uint8_t, 7> chunk = {
        0x00, 0x00, 0x01, 0x00,
        0x01,
        0x00, 0x0A,
    };
    auto r = flc::decode_lc(chunk, fb.data(), 4, 4, 4);
    CHECK_FALSE(r);
}

TEST_CASE("decode_ss2 line skip + literal word") {
    constexpr unsigned int W = 8, H = 4;
    std::array<std::uint8_t, W * H> fb{};
    fb.fill(0xCC);

    // Pseudocode of opcode stream:
    //   line_count = 1
    //   line: opcode 0xFFFE → tag=11, signed -2 → skip 2 lines (y becomes 2)
    //         opcode 0x0001 → tag=00, packet_count = 1
    //         packet: col_skip=2, rle=+2 (literal 2 words = 4 bytes)
    //         data: 0xAA 0xBB 0xCC 0xDD
    //   then y → 3 (post-line)
    constexpr std::array<std::uint8_t, 12> chunk = {
        0x01, 0x00,                       // line_count = 1
        0xFE, 0xFF,                       // opcode: line skip = 2
        0x01, 0x00,                       // opcode: packet_count = 1
        0x02, 0x02,                       // col_skip=2, rle=+2 (words)
        0xAA, 0xBB, 0xCC, 0xDD,
    };

    auto r = flc::decode_ss2(chunk, fb.data(), W, W, H);
    REQUIRE(r);
    // Rows 0,1 untouched
    for (unsigned x = 0; x < W; ++x) CHECK(fb[0 * W + x] == 0xCC);
    for (unsigned x = 0; x < W; ++x) CHECK(fb[1 * W + x] == 0xCC);
    // Row 2: cols 2..5 = AA BB CC DD; cols 0,1,6,7 untouched
    CHECK(fb[2 * W + 0] == 0xCC);
    CHECK(fb[2 * W + 1] == 0xCC);
    CHECK(fb[2 * W + 2] == 0xAA);
    CHECK(fb[2 * W + 3] == 0xBB);
    CHECK(fb[2 * W + 4] == 0xCC);
    CHECK(fb[2 * W + 5] == 0xDD);
    CHECK(fb[2 * W + 6] == 0xCC);
    CHECK(fb[2 * W + 7] == 0xCC);
    // Row 3: untouched
    CHECK(fb[3 * W + 0] == 0xCC);
}

TEST_CASE("decode_ss2 replicate run") {
    constexpr unsigned int W = 6, H = 1;
    std::array<std::uint8_t, W * H> fb{};

    // line_count = 1, no skip
    // opcode: packet_count = 1
    // packet: col_skip=0, rle=-2 (replicate 2 copies of one word 0x77 0x88)
    constexpr std::array<std::uint8_t, 8> chunk = {
        0x01, 0x00,
        0x01, 0x00,
        0x00, 0xFE,    // skip=0, rle = -2
        0x77, 0x88,
    };
    auto r = flc::decode_ss2(chunk, fb.data(), W, W, H);
    REQUIRE(r);
    CHECK(fb[0] == 0x77);
    CHECK(fb[1] == 0x88);
    CHECK(fb[2] == 0x77);
    CHECK(fb[3] == 0x88);
    CHECK(fb[4] == 0x00);
    CHECK(fb[5] == 0x00);
}

TEST_CASE("decode_ss2 last-byte opcode stores final pixel") {
    constexpr unsigned int W = 5, H = 1;
    std::array<std::uint8_t, W * H> fb{};
    fb.fill(0x11);

    // line_count = 1
    // line opcodes:
    //   0x80AA → tag=10, low byte 0xAA stored at column W-1 = 4
    //   0x0000 → tag=00, packet_count = 0 (no packets follow)
    constexpr std::array<std::uint8_t, 6> chunk = {
        0x01, 0x00,
        0xAA, 0x80,
        0x00, 0x00,
    };
    auto r = flc::decode_ss2(chunk, fb.data(), W, W, H);
    REQUIRE(r);
    CHECK(fb[0] == 0x11);
    CHECK(fb[1] == 0x11);
    CHECK(fb[2] == 0x11);
    CHECK(fb[3] == 0x11);
    CHECK(fb[4] == 0xAA);
}

TEST_CASE("decode_ss2 rejects undefined opcode tag (01)") {
    std::array<std::uint8_t, 16> fb{};
    constexpr std::array<std::uint8_t, 4> chunk = {
        0x01, 0x00,
        0x00, 0x40,    // tag = 0b01 → undefined
    };
    auto r = flc::decode_ss2(chunk, fb.data(), 4, 4, 4);
    CHECK_FALSE(r);
}

TEST_CASE("decode_color_256 writes RGB triplets at the right offsets") {
    std::array<std::uint8_t, flc::kPaletteBytes> pal{};
    pal.fill(0xAB);

    // Two packets:
    //   skip 2 entries, write 1 RGB (0x10, 0x20, 0x30)  → entry 2
    //   skip 4 entries, write 2 RGB (red, green)        → entries 7 and 8
    constexpr std::array<std::uint8_t, 15> chunk2 = {
        0x02, 0x00,                       // packet count = 2
        0x02, 0x01, 0x10, 0x20, 0x30,     // skip=2, copy=1: entry 2 = (10,20,30)
        0x04, 0x02,                       // skip=4, copy=2 → next two entries at idx 7,8
        0xFF, 0x00, 0x00,                 // entry 7 = red
        0x00, 0xFF, 0x00,                 // entry 8 = green
    };
    auto r = flc::decode_color_256(chunk2, pal.data());
    REQUIRE(r);
    // Entries 0,1 untouched
    CHECK(pal[0 * 3 + 0] == 0xAB);
    CHECK(pal[1 * 3 + 0] == 0xAB);
    // Entry 2 = (10,20,30)
    CHECK(pal[2 * 3 + 0] == 0x10);
    CHECK(pal[2 * 3 + 1] == 0x20);
    CHECK(pal[2 * 3 + 2] == 0x30);
    // Entries 3..6 untouched
    CHECK(pal[3 * 3 + 0] == 0xAB);
    CHECK(pal[6 * 3 + 0] == 0xAB);
    // Entry 7 = red
    CHECK(pal[7 * 3 + 0] == 0xFF);
    CHECK(pal[7 * 3 + 1] == 0x00);
    CHECK(pal[7 * 3 + 2] == 0x00);
    // Entry 8 = green
    CHECK(pal[8 * 3 + 1] == 0xFF);
}

TEST_CASE("decode_color_64 scales 6-bit values to full 0-255 range") {
    std::array<std::uint8_t, flc::kPaletteBytes> pal{};

    // One packet: skip 0, copy 1, RGB = (63, 0, 32) (max, min, mid)
    constexpr std::array<std::uint8_t, 7> chunk = {
        0x01, 0x00,         // packet count = 1
        0x00, 0x01,         // skip=0, copy=1
        63, 0, 32,
    };
    auto r = flc::decode_color_64(chunk, pal.data());
    REQUIRE(r);
    // 63 → 255 (replicate shift: (63<<2)|(63>>4) = 252|3 = 255)
    CHECK(pal[0] == 255);
    CHECK(pal[1] == 0);
    // 32 → (32<<2)|(32>>4) = 128|2 = 130
    CHECK(pal[2] == 130);
}

TEST_CASE("decode_color_256 copy_count zero means 256 entries") {
    std::array<std::uint8_t, flc::kPaletteBytes> pal{};
    std::array<std::uint8_t, 2 + 2 + 256 * 3> chunk{};
    chunk[0] = 0x01; chunk[1] = 0x00;     // 1 packet
    chunk[2] = 0x00;                      // skip=0
    chunk[3] = 0x00;                      // copy=0 → 256 triplets
    for (unsigned int i = 0; i < 256; ++i) {
        chunk[4 + i*3 + 0] = static_cast<std::uint8_t>(i);
        chunk[4 + i*3 + 1] = static_cast<std::uint8_t>(255 - i);
        chunk[4 + i*3 + 2] = 0x55;
    }
    auto r = flc::decode_color_256(chunk, pal.data());
    REQUIRE(r);
    CHECK(pal[0]   == 0);
    CHECK(pal[1]   == 255);
    CHECK(pal[2]   == 0x55);
    CHECK(pal[255*3 + 0] == 255);
    CHECK(pal[255*3 + 1] == 0);
    CHECK(pal[255*3 + 2] == 0x55);
}

TEST_CASE("decode_color_256 rejects packet that overflows palette") {
    std::array<std::uint8_t, flc::kPaletteBytes> pal{};
    // skip=255, copy=10 → would write past entry 255
    constexpr std::array<std::uint8_t, 6 + 30> chunk = {
        0x01, 0x00,
        0xFF, 0x0A,
        // 30 bytes of payload
        0,0,0, 0,0,0, 0,0,0, 0,0,0, 0,0,0,
        0,0,0, 0,0,0, 0,0,0, 0,0,0, 0,0,0,
    };
    auto r = flc::decode_color_256(chunk, pal.data());
    CHECK_FALSE(r);
}

TEST_CASE("decode_color_256 detects truncated payload") {
    std::array<std::uint8_t, flc::kPaletteBytes> pal{};
    // Claims 1 packet, skip=0, copy=2 (= 6 bytes payload), but only 3 bytes follow
    constexpr std::array<std::uint8_t, 7> chunk = {
        0x01, 0x00,
        0x00, 0x02,
        0xFF, 0x00, 0x00,
    };
    auto r = flc::decode_color_256(chunk, pal.data());
    CHECK_FALSE(r);
}
