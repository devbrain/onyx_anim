#include <doctest/doctest.h>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <onyx_image/surface.hpp>

#include <musac/sdk/io_stream.hh>

#include <filesystem>
#include <string>

namespace {

std::string sample(const char* filename) {
    return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/yafa/" + filename;
}

bool exists(const std::string& p) {
    return std::filesystem::exists(p);
}

} // namespace

TEST_CASE("yafa: sniff + open + first-frame decode covers all compression modes") {
    // Files exercising the four code paths the codec needs to handle:
    //   - wuerfel.yafa : CHUNKY8 + XPK FAST + dynamic palette
    //   - underwater.yafa : CHUNKY8 + XPK FAST + global palette (DRGB)
    //   - b.yafa       : CHUNKY8 + XPK NUKE + dynamic palette
    //   - a.yafa       : PLANAR + byte-delta (no XPK)
    struct sample_case {
        const char*  name;
        unsigned int width;
        unsigned int height;
        unsigned int min_frames;
    };
    constexpr sample_case cases[] = {
        {"wuerfel.yafa",    320, 256, 100},  // FAST + dyn palette
        {"underwater.yafa", 176, 140, 421},  // FAST + global palette
        {"b.yafa",          160, 128,  36},  // NUKE
        {"a.yafa",           80,  64,  36},  // byte-delta planar
    };

    auto& reg = onyx_anim::codec_registry::instance();

    for (const auto& c : cases) {
        const auto path = sample(c.name);
        if (!exists(path)) {
            MESSAGE("skipping YAFA sample (missing): " << c.name);
            continue;
        }

        auto stream = musac::io_from_file(path.c_str(), "rb");
        REQUIRE_MESSAGE(stream, "could not open ", c.name);

        auto dec = reg.create_decoder(stream.get());
        REQUIRE_MESSAGE(dec, "no codec sniffed ", c.name);
        CHECK(dec->name() == "yafa");

        REQUIRE(dec->open(stream.get()));
        const auto& info = dec->info();
        CHECK(info.width  == c.width);
        CHECK(info.height == c.height);
        CHECK(info.frame_count >= c.min_frames);
        CHECK(info.frame_period.count() > 0);

        onyx_image::memory_surface surf;
        auto fr = dec->decode_frame(surf);
        REQUIRE(fr);
        CHECK(static_cast<unsigned int>(surf.width())  == c.width);
        CHECK(static_cast<unsigned int>(surf.height()) == c.height);
    }
}

TEST_CASE("yafa: full playthrough of a compressed sample") {
    // Mid-stream frames are where bugs surface (delta state, XPK chunk
    // boundaries, etc.). Walk every frame of the longest FAST sample.
    const auto path = sample("subspace.yafa");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream);
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    onyx_image::memory_surface surf;
    int decoded = 0;
    while (!dec->eof()) {
        auto fr = dec->decode_frame(surf);
        if (!fr) break;
        ++decoded;
    }
    CHECK(decoded == static_cast<int>(dec->info().frame_count));
}
