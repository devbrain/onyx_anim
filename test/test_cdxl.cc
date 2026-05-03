#include <doctest/doctest.h>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <onyx_image/surface.hpp>

#include <musac/sdk/io_stream.hh>

#include <filesystem>
#include <string>

namespace {

std::string sample(const char* filename) {
    return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/cdxl/" + filename;
}

bool exists(const std::string& p) {
    return std::filesystem::exists(p);
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
