#include <doctest/doctest.h>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>
#include <onyx_image/surface.hpp>

#include <musac/sdk/io_stream.hh>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Tiny in-memory FLC builder. Produces a 4x2 single-frame FLC with one
// COLOR_256 sub-chunk (sets palette[1] = red) and one BRUN sub-chunk
// (fills the whole frame with palette index 1).
// -----------------------------------------------------------------------------

void put_u16le(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
}
void put_u32le(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 8)  & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFFu));
}

std::vector<std::uint8_t> build_minimal_flc() {
    constexpr std::uint16_t W = 4, H = 2;

    // ---- Sub-chunk 1: COLOR_256 — set entry 1 to (0xFF, 0x00, 0x00).
    std::vector<std::uint8_t> sub_color;
    // payload: 1 packet, skip=1, copy=1, 0xFF 0x00 0x00
    put_u16le(sub_color, 1);          // packet_count
    sub_color.push_back(0x01);        // skip = 1
    sub_color.push_back(0x01);        // copy = 1
    sub_color.push_back(0xFF);
    sub_color.push_back(0x00);
    sub_color.push_back(0x00);
    const std::uint32_t color_size = 6 + static_cast<std::uint32_t>(sub_color.size());

    // ---- Sub-chunk 2: BRUN — each line "replicate 4 of value 0x01".
    std::vector<std::uint8_t> sub_brun;
    for (unsigned int y = 0; y < H; ++y) {
        sub_brun.push_back(0x01);     // obsolete opcount
        sub_brun.push_back(static_cast<std::uint8_t>(W));  // count = +4 → replicate
        sub_brun.push_back(0x01);     // pixel value
    }
    const std::uint32_t brun_size = 6 + static_cast<std::uint32_t>(sub_brun.size());

    // ---- Frame chunk: 16-byte header + the two sub-chunks.
    const std::uint32_t frame_size = 16 + color_size + brun_size;

    // ---- File header (128 bytes).
    const std::uint32_t file_size = 128 + frame_size;

    std::vector<std::uint8_t> out;
    out.reserve(file_size);

    // ---- file header (we only fill what the parser cares about).
    put_u32le(out, file_size);                 // size
    put_u16le(out, 0xAF12);                    // magic = FLC
    put_u16le(out, 1);                         // frame_count = 1
    put_u16le(out, W);                         // width
    put_u16le(out, H);                         // height
    put_u16le(out, 8);                         // depth
    put_u16le(out, 3);                         // flags
    put_u32le(out, 33);                        // speed_units (ms/frame for FLC)
    put_u16le(out, 0);                         // reserved1
    put_u32le(out, 0);                         // created
    put_u32le(out, 0);                         // creator
    put_u32le(out, 0);                         // updated
    put_u32le(out, 0);                         // updater
    put_u16le(out, 1);                         // aspect_dx
    put_u16le(out, 1);                         // aspect_dy
    put_u16le(out, 0);                         // ext_flags
    put_u16le(out, 0);                         // keyframes
    put_u16le(out, 1);                         // totalframes
    put_u32le(out, 0);                         // req_memory
    put_u16le(out, 0);                         // max_regions
    put_u16le(out, 0);                         // transp_num
    out.insert(out.end(), 24, 0);              // reserved2
    put_u32le(out, 128);                       // oframe1
    put_u32le(out, 128 + frame_size);          // oframe2 (no second frame; harmless)
    out.insert(out.end(), 40, 0);              // reserved3
    REQUIRE(out.size() == 128);

    // ---- Frame chunk header (16 bytes).
    put_u32le(out, frame_size);                // chunk size
    put_u16le(out, 0xF1FA);                    // magic
    put_u16le(out, 2);                         // sub_chunks
    put_u16le(out, 0);                         // delay (use file speed)
    put_u16le(out, 0);                         // reserved
    put_u16le(out, 0);                         // width override
    put_u16le(out, 0);                         // height override

    // ---- COLOR_256 sub-chunk header + payload.
    put_u32le(out, color_size);
    put_u16le(out, 4);                         // type 4 = COLOR_256
    out.insert(out.end(), sub_color.begin(), sub_color.end());

    // ---- BRUN sub-chunk header + payload.
    put_u32le(out, brun_size);
    put_u16le(out, 15);                        // type 15 = BYTE_RUN
    out.insert(out.end(), sub_brun.begin(), sub_brun.end());

    REQUIRE(out.size() == file_size);
    return out;
}

} // namespace

// Two-frame FLC: frame 0 fills the framebuffer with index 1 (BRUN keyframe),
// frame 1 applies an LC delta that overwrites the middle two pixels of row 0
// with index 2. Verifies that delta decoding reads the persistent framebuffer
// state from the previous decode_frame call.
std::vector<std::uint8_t> build_two_frame_flc() {
    constexpr std::uint16_t W = 4, H = 1;

    // ---- frame 0: COLOR_256 (entries 1=red, 2=green) + BRUN (fill with 1)
    std::vector<std::uint8_t> color_payload;
    put_u16le(color_payload, 1);          // 1 packet
    color_payload.push_back(0x01);        // skip=1
    color_payload.push_back(0x02);        // copy=2
    color_payload.push_back(0xFF); color_payload.push_back(0x00); color_payload.push_back(0x00); // entry 1
    color_payload.push_back(0x00); color_payload.push_back(0xFF); color_payload.push_back(0x00); // entry 2
    const std::uint32_t color_size = 6 + static_cast<std::uint32_t>(color_payload.size());

    std::vector<std::uint8_t> brun_payload = {0x01, 0x04, 0x01}; // 1 line: replicate 4× value 1
    const std::uint32_t brun_size = 6 + static_cast<std::uint32_t>(brun_payload.size());

    const std::uint32_t frame0_size = 16 + color_size + brun_size;

    // ---- frame 1: LC delta — line_skip=0, line_count=1, packet col_skip=1, rle=-2 (replicate 2× value 2)
    std::vector<std::uint8_t> lc_payload = {
        0x00, 0x00,         // line_skip = 0
        0x01, 0x00,         // line_count = 1
        0x01,               // packet_count = 1
        0x01, 0xFE, 0x02,   // col_skip=1, rle=-2 (replicate), value=2
    };
    const std::uint32_t lc_size = 6 + static_cast<std::uint32_t>(lc_payload.size());
    const std::uint32_t frame1_size = 16 + lc_size;

    const std::uint32_t file_size = 128 + frame0_size + frame1_size;

    std::vector<std::uint8_t> out;
    out.reserve(file_size);

    // file header
    put_u32le(out, file_size);
    put_u16le(out, 0xAF12);
    put_u16le(out, 2); // frame_count = 2
    put_u16le(out, W); put_u16le(out, H);
    put_u16le(out, 8); put_u16le(out, 3);
    put_u32le(out, 33);
    put_u16le(out, 0);
    put_u32le(out, 0); put_u32le(out, 0); put_u32le(out, 0); put_u32le(out, 0);
    put_u16le(out, 1); put_u16le(out, 1);
    put_u16le(out, 0); put_u16le(out, 0); put_u16le(out, 2); // ext_flags, keyframes, totalframes
    put_u32le(out, 0); // req_memory
    put_u16le(out, 0); put_u16le(out, 0);
    out.insert(out.end(), 24, 0);
    put_u32le(out, 128);
    put_u32le(out, 128 + frame0_size);
    out.insert(out.end(), 40, 0);
    REQUIRE(out.size() == 128);

    // frame 0
    put_u32le(out, frame0_size);
    put_u16le(out, 0xF1FA);
    put_u16le(out, 2); // sub_chunks
    put_u16le(out, 0); put_u16le(out, 0); put_u16le(out, 0); put_u16le(out, 0);
    put_u32le(out, color_size); put_u16le(out, 4);
    out.insert(out.end(), color_payload.begin(), color_payload.end());
    put_u32le(out, brun_size); put_u16le(out, 15);
    out.insert(out.end(), brun_payload.begin(), brun_payload.end());

    // frame 1
    put_u32le(out, frame1_size);
    put_u16le(out, 0xF1FA);
    put_u16le(out, 1); // sub_chunks
    put_u16le(out, 0); put_u16le(out, 0); put_u16le(out, 0); put_u16le(out, 0);
    put_u32le(out, lc_size); put_u16le(out, 12);
    out.insert(out.end(), lc_payload.begin(), lc_payload.end());

    REQUIRE(out.size() == file_size);
    return out;
}

TEST_CASE("seek_to_frame: forward seek skips state-only through deltas") {
    const auto bytes = build_two_frame_flc();
    auto stream = musac::io_from_memory(bytes.data(), bytes.size());
    auto dec = onyx_anim::codec_registry::instance().create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    // Jump straight to frame 1 without rendering frame 0.
    REQUIRE(dec->seek_to_frame(1));

    // The next decode_frame() should emit frame 1 — but its persistent
    // framebuffer must already reflect frame 0's contents (the delta only
    // touches columns 1..2; cols 0 and 3 must come from frame 0).
    onyx_image::memory_surface s;
    auto fr = dec->decode_frame(s);
    REQUIRE(fr);
    CHECK(fr->index == 1);
    REQUIRE(s.pixels().size() >= 4);
    CHECK(s.pixels()[0] == 1);   // from frame 0 BRUN
    CHECK(s.pixels()[1] == 2);   // delta
    CHECK(s.pixels()[2] == 2);   // delta
    CHECK(s.pixels()[3] == 1);   // from frame 0 BRUN
}

TEST_CASE("seek_to_frame: backward seek replays from the start") {
    const auto bytes = build_two_frame_flc();
    auto stream = musac::io_from_memory(bytes.data(), bytes.size());
    auto dec = onyx_anim::codec_registry::instance().create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    onyx_image::memory_surface s;
    REQUIRE(dec->decode_frame(s));   // frame 0
    REQUIRE(dec->decode_frame(s));   // frame 1
    REQUIRE(dec->eof());

    REQUIRE(dec->seek_to_frame(0));
    CHECK_FALSE(dec->eof());

    auto fr = dec->decode_frame(s);
    REQUIRE(fr);
    CHECK(fr->index == 0);
    REQUIRE(s.pixels().size() >= 4);
    for (std::size_t i = 0; i < 4; ++i) CHECK(s.pixels()[i] == 1);
}

TEST_CASE("seek_to_frame: idempotent at current cursor position") {
    const auto bytes = build_two_frame_flc();
    auto stream = musac::io_from_memory(bytes.data(), bytes.size());
    auto dec = onyx_anim::codec_registry::instance().create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    REQUIRE(dec->seek_to_frame(0));   // cursor was 0; no-op
    REQUIRE(dec->seek_to_frame(0));   // still 0
    onyx_image::memory_surface s;
    auto fr = dec->decode_frame(s);
    REQUIRE(fr);
    CHECK(fr->index == 0);
}

TEST_CASE("seek_to_frame: out-of-range index fails") {
    const auto bytes = build_two_frame_flc();
    auto stream = musac::io_from_memory(bytes.data(), bytes.size());
    auto dec = onyx_anim::codec_registry::instance().create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    CHECK_FALSE(dec->seek_to_frame(99));
}

TEST_CASE("rewind: equivalent to seek_to_frame(0)") {
    const auto bytes = build_two_frame_flc();
    auto stream = musac::io_from_memory(bytes.data(), bytes.size());
    auto dec = onyx_anim::codec_registry::instance().create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    onyx_image::memory_surface s;
    REQUIRE(dec->decode_frame(s));
    REQUIRE(dec->decode_frame(s));
    REQUIRE(dec->rewind());
    auto fr = dec->decode_frame(s);
    REQUIRE(fr);
    CHECK(fr->index == 0);
}

TEST_CASE("seek_to_time: maps PTS to frame index via frame_period") {
    const auto bytes = build_two_frame_flc();
    auto stream = musac::io_from_memory(bytes.data(), bytes.size());
    auto dec = onyx_anim::codec_registry::instance().create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    // Frame period for a 33ms FLC is 33000 microseconds.
    // PTS=0 → frame 0
    // PTS=33000 → frame 1
    REQUIRE(dec->seek_to_time(std::chrono::microseconds{33'000}));
    onyx_image::memory_surface s;
    auto fr = dec->decode_frame(s);
    REQUIRE(fr);
    CHECK(fr->index == 1);
}

TEST_CASE("seek_to_time: negative PTS snaps to frame 0") {
    const auto bytes = build_two_frame_flc();
    auto stream = musac::io_from_memory(bytes.data(), bytes.size());
    auto dec = onyx_anim::codec_registry::instance().create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    REQUIRE(dec->seek_to_time(std::chrono::microseconds{-1'000'000}));
    onyx_image::memory_surface s;
    auto fr = dec->decode_frame(s);
    REQUIRE(fr);
    CHECK(fr->index == 0);
}

TEST_CASE("end-to-end: two-frame FLC with LC delta accumulates state") {
    const auto bytes = build_two_frame_flc();
    auto stream = musac::io_from_memory(bytes.data(), bytes.size());

    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    onyx_image::memory_surface s0, s1;

    auto fr0 = dec->decode_frame(s0);
    REQUIRE(fr0);
    CHECK(fr0->index == 0);
    REQUIRE(s0.pixels().size() >= 4);
    for (std::size_t i = 0; i < 4; ++i) CHECK(s0.pixels()[i] == 1);

    auto fr1 = dec->decode_frame(s1);
    REQUIRE(fr1);
    CHECK(fr1->index == 1);
    // Expected row after delta: [1, 2, 2, 1]
    REQUIRE(s1.pixels().size() >= 4);
    CHECK(s1.pixels()[0] == 1);   // untouched, from prior frame's persistent fb
    CHECK(s1.pixels()[1] == 2);   // delta: replicated value
    CHECK(s1.pixels()[2] == 2);
    CHECK(s1.pixels()[3] == 1);   // untouched

    CHECK(dec->eof());
}

TEST_CASE("end-to-end: decode a minimal FLC frame through the public API") {
    const auto bytes = build_minimal_flc();
    auto stream = musac::io_from_memory(bytes.data(), bytes.size());

    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->name() == "flc");

    auto opened = dec->open(stream.get());
    REQUIRE(opened);

    const auto& info = dec->info();
    CHECK(info.width == 4);
    CHECK(info.height == 2);
    CHECK(info.frame_count == 1);
    CHECK(info.format == onyx_anim::pixel_format::indexed8);

    onyx_image::memory_surface surf;
    auto fr = dec->decode_frame(surf);
    REQUIRE(fr);
    CHECK(fr->index == 0);
    CHECK(fr->palette_changed);

    // 4x2 indexed8 surface; every pixel should be palette index 1.
    REQUIRE(surf.width() == 4);
    REQUIRE(surf.height() == 2);
    auto pixels = surf.pixels();
    REQUIRE(pixels.size() >= 8);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(pixels[i] == 0x01);
    }

    // Palette entry 1 = (0xFF, 0x00, 0x00).
    auto pal = surf.palette();
    REQUIRE(pal.size() >= 6);
    CHECK(pal[3] == 0xFF);
    CHECK(pal[4] == 0x00);
    CHECK(pal[5] == 0x00);

    // After the only frame, eof() is true.
    CHECK(dec->eof());
}
