#include <doctest/doctest.h>

#include <flc/header.hh>

#include <array>
#include <cstdint>

namespace {
    // Full 88-byte FLC header (the documented extended layout, sans the 40
    // trailing reserved3 bytes which the parser doesn't consume).
    //
    //   [0..3]   size         = 0x00010000
    //   [4..5]   magic        = 0xAF12 (FLC)
    //   [6..7]   frames       = 100
    //   [8..9]   width        = 320
    //   [10..11] height       = 200
    //   [12..13] depth        = 8
    //   [14..15] flags        = 3
    //   [16..19] speed        = 33  (ms/frame)
    //   [20..21] reserved1    = 0
    //   [22..25] created      = 0x12345678
    //   [26..29] creator      = 0x9ABCDEF0
    //   [30..33] updated      = 0x11111111
    //   [34..37] updater      = 0x22222222
    //   [38..39] aspect_dx    = 4
    //   [40..41] aspect_dy    = 3
    //   [42..43] ext_flags    = 0
    //   [44..45] keyframes    = 10
    //   [46..47] totalframes  = 100
    //   [48..51] req_memory   = 1024  (DWORD)
    //   [52..53] max_regions  = 8
    //   [54..55] transp_num   = 0
    //   [56..79] reserved2    = 24 zero bytes
    //   [80..83] oframe1      = 0x80
    //   [84..87] oframe2      = 0x100
    constexpr std::array<std::uint8_t, 88> kValidFlc = {
        0x00, 0x00, 0x01, 0x00,                         // size
        0x12, 0xAF,                                     // magic FLC
        0x64, 0x00,                                     // frame_count
        0x40, 0x01, 0xC8, 0x00,                         // width, height
        0x08, 0x00, 0x03, 0x00,                         // depth, flags
        0x21, 0x00, 0x00, 0x00,                         // speed_units
        0x00, 0x00,                                     // reserved1
        0x78, 0x56, 0x34, 0x12,                         // created
        0xF0, 0xDE, 0xBC, 0x9A,                         // creator
        0x11, 0x11, 0x11, 0x11,                         // updated
        0x22, 0x22, 0x22, 0x22,                         // updater
        0x04, 0x00, 0x03, 0x00,                         // aspect_dx, aspect_dy
        0x00, 0x00, 0x0A, 0x00,                         // ext_flags, keyframes
        0x64, 0x00,                                     // totalframes
        0x00, 0x04, 0x00, 0x00,                         // req_memory (DWORD)
        0x08, 0x00, 0x00, 0x00,                         // max_regions, transp_num
        0,0,0,0, 0,0,0,0, 0,0,0,0,                      // reserved2 (24 bytes)
        0,0,0,0, 0,0,0,0, 0,0,0,0,
        0x80, 0x00, 0x00, 0x00,                         // oframe1
        0x00, 0x01, 0x00, 0x00,                         // oframe2
    };

    constexpr std::array<std::uint8_t, 24> kValidFli = {
        0x00, 0x00, 0x01, 0x00,
        0x11, 0xAF,               // magic FLI
        0x32, 0x00,               // frames = 50
        0x40, 0x01,
        0xC8, 0x00,
        0x08, 0x00,
        0x00, 0x00,
        0x05, 0x00, 0x00, 0x00,   // speed = 5 jiffies
    };
} // namespace

TEST_CASE("parse_file_header accepts FLC magic and reads basic fields") {
    auto h = flc::parse_file_header(kValidFlc);
    REQUIRE(h);
    CHECK(h->magic == flc::kMagicFlc);
    CHECK(h->is_flc());
    CHECK_FALSE(h->is_fli());
    CHECK(h->frame_count == 100);
    CHECK(h->width == 320);
    CHECK(h->height == 200);
    CHECK(h->depth == 8);
    CHECK(h->flags == 3);
    CHECK(h->speed_units == 33);
    CHECK(h->size == 0x10000u);
}

TEST_CASE("parse_file_header reads FLC extended fields") {
    auto h = flc::parse_file_header(kValidFlc);
    REQUIRE(h);
    CHECK(h->reserved1   == 0);
    CHECK(h->created     == 0x12345678u);
    CHECK(h->creator     == 0x9ABCDEF0u);
    CHECK(h->updated     == 0x11111111u);
    CHECK(h->updater     == 0x22222222u);
    CHECK(h->aspect_dx   == 4);
    CHECK(h->aspect_dy   == 3);
    CHECK(h->ext_flags   == 0);
    CHECK(h->keyframes   == 10);
    CHECK(h->totalframes == 100);
    CHECK(h->req_memory  == 1024);
    CHECK(h->max_regions == 8);
    CHECK(h->transp_num  == 0);
    CHECK(h->oframe1     == 0x80u);
    CHECK(h->oframe2     == 0x100u);
}

TEST_CASE("parse_file_header on FLI leaves extended fields zero") {
    auto h = flc::parse_file_header(kValidFli);
    REQUIRE(h);
    CHECK(h->is_fli());
    CHECK(h->frame_count == 50);
    CHECK(h->speed_units == 5);
    // FLI files don't carry the FLC-only extended block.
    CHECK(h->created == 0u);
    CHECK(h->oframe1 == 0u);
    CHECK(h->aspect_dx == 0);
}

TEST_CASE("parse_file_header rejects FLC missing extended header") {
    // Truncate the FLC fixture down to just the basic 24 bytes; that's enough
    // to clear the FLI minimum but not enough to read FLC's extended block.
    std::array<std::uint8_t, 24> short_flc{};
    for (std::size_t i = 0; i < short_flc.size(); ++i) short_flc[i] = kValidFlc[i];
    auto h = flc::parse_file_header(short_flc);
    CHECK_FALSE(h);
}

TEST_CASE("parse_file_header rejects truncated input") {
    std::array<std::uint8_t, 8> truncated{};
    auto h = flc::parse_file_header(truncated);
    CHECK_FALSE(h);
}

TEST_CASE("parse_file_header rejects bad magic") {
    auto bad = kValidFlc;
    bad[4] = 0xFF;
    bad[5] = 0xFF;
    auto h = flc::parse_file_header(bad);
    CHECK_FALSE(h);
}

TEST_CASE("parse_frame_header accepts standard frame") {
    constexpr std::array<std::uint8_t, 16> frame = {
        0x40, 0x00, 0x00, 0x00,   // size = 64
        0xFA, 0xF1,               // magic = 0xF1FA
        0x02, 0x00,               // sub_chunks = 2
        0,0,0,0,0,0,0,0,
    };
    auto h = flc::parse_frame_header(frame);
    REQUIRE(h);
    CHECK(h->magic == flc::kFrameMagicStandard);
    CHECK(h->size == 64);
    CHECK(h->sub_chunks == 2);
    CHECK(h->delay == 0);
    CHECK(h->width == 0);
    CHECK(h->height == 0);
}

TEST_CASE("parse_frame_header reads per-frame delay and size override") {
    constexpr std::array<std::uint8_t, 16> frame = {
        0x40, 0x00, 0x00, 0x00,   // size = 64
        0xFA, 0xF1,               // magic = 0xF1FA
        0x03, 0x00,               // sub_chunks = 3
        0x21, 0x00,               // delay = 33 ms (Pro Motion ext.)
        0x00, 0x00,               // reserved
        0x40, 0x01,               // width  override = 320
        0xC8, 0x00,               // height override = 200
    };
    auto h = flc::parse_frame_header(frame);
    REQUIRE(h);
    CHECK(h->sub_chunks == 3);
    CHECK(h->delay == 33);
    CHECK(h->width == 320);
    CHECK(h->height == 200);
}

TEST_CASE("parse_frame_header rejects unknown magic") {
    std::array<std::uint8_t, 16> frame = {
        0x10, 0x00, 0x00, 0x00,
        0xAA, 0xBB,
        0x00, 0x00,
        0,0,0,0,0,0,0,0,
    };
    auto h = flc::parse_frame_header(frame);
    CHECK_FALSE(h);
}
