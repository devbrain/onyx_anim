#include <doctest/doctest.h>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <onyx_image/surface.hpp>

#include <musac/sdk/io_stream.hh>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string sample(const char* filename) {
    return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/cdxl/" + filename;
}

bool exists(const std::string& p) {
    return std::filesystem::exists(p);
}

void put_u16be(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>(x & 0xFFu));
}

void put_u32be(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>(x & 0xFFu));
}

void append_cdxl_header(std::vector<std::uint8_t>& v,
                        std::uint32_t chunk_size,
                        std::uint16_t width,
                        std::uint16_t height,
                        std::uint8_t planes) {
    v.push_back(0x01);                // STANDARD
    v.push_back(0x00);                // RGB, BIT_PLANAR
    put_u32be(v, chunk_size);
    put_u32be(v, chunk_size);
    put_u32be(v, 0);
    put_u16be(v, width);
    put_u16be(v, height);
    v.push_back(0);
    v.push_back(planes);
    put_u16be(v, 0);                  // cmap bytes
    put_u16be(v, 0);                  // audio bytes
    v.insert(v.end(), 8, 0);
}

} // namespace

TEST_CASE("cdxl: sniff + open + decode frame on representative samples") {
    // Three files exercising the decoder's main paths:
    //   - Discovery.CDXL : 8-plane RGB indexed, 68×68, with audio
    //   - Spot.CDXL      : HAM6 BIT_PLANAR 160×100, with audio
    //   - logo.xl        : HAM6 225×128, *no* audio
    struct sample_case {
        const char*  name;
        unsigned int width;
        unsigned int height;
        unsigned int frame_count_hint;  // approximate, file may differ
        bool         expect_audio;
    };
    constexpr sample_case cases[] = {
        {"Discovery.CDXL", 68,  68,  90,  true},
        {"Spot.CDXL",      160, 100, 360, true},
        {"logo.xl",        225, 128, 660, false},
    };

    auto& reg = onyx_anim::codec_registry::instance();

    for (const auto& c : cases) {
        const auto path = sample(c.name);
        if (!exists(path)) {
            MESSAGE("skipping CDXL sample (missing): " << c.name);
            continue;
        }

        auto stream = musac::io_from_file(path.c_str(), "rb");
        REQUIRE_MESSAGE(stream, "could not open ", c.name);

        auto dec = reg.create_decoder(stream.get());
        REQUIRE_MESSAGE(dec, "no codec sniffed ", c.name);
        CHECK(dec->name() == "cdxl");

        REQUIRE(dec->open(stream.get()));
        const auto& info = dec->info();
        CHECK(info.width  == c.width);
        CHECK(info.height == c.height);
        CHECK(info.frame_count > 0u);
        CHECK(info.frame_period.count() > 0);
        CHECK(info.audio_track_count == (c.expect_audio ? 1u : 0u));

        // Decode the first frame and verify the surface receives data of
        // the expected size.
        onyx_image::memory_surface surf;
        auto fr = dec->decode_frame(surf);
        REQUIRE(fr);
        CHECK(static_cast<unsigned int>(surf.width())  == c.width);
        CHECK(static_cast<unsigned int>(surf.height()) == c.height);
    }
}

TEST_CASE("cdxl: BIT_LINE pixel orientation is rejected at open()") {
    const auto path = sample("optologo.cdxl");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream);
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    // Sniff is permissive (BIT_LINE files look like CDXL by header), but
    // open() rejects layouts we don't decode.
    REQUIRE(dec);
    auto rc = dec->open(stream.get());
    CHECK_FALSE(rc);
}

TEST_CASE("cdxl: mid-stream geometry changes are rejected") {
    constexpr std::uint32_t chunk_size = 36; // 32-byte header + 2x2x1 bitplane data
    std::vector<std::uint8_t> bytes;
    bytes.reserve(chunk_size * 2u);

    append_cdxl_header(bytes, chunk_size, 2, 2, 1);
    bytes.insert(bytes.end(), 4, 0);
    append_cdxl_header(bytes, chunk_size, 4, 2, 1);
    bytes.insert(bytes.end(), 4, 0);

    auto stream = musac::io_from_memory(bytes.data(), bytes.size());
    REQUIRE(stream);
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder("cdxl");
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    onyx_image::memory_surface surf;
    REQUIRE(dec->decode_frame(surf));
    CHECK_FALSE(dec->decode_frame(surf));
}
