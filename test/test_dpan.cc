#include <doctest/doctest.h>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <onyx_image/surface.hpp>

#include <musac/sdk/io_stream.hh>

#include <filesystem>
#include <string>

namespace {

std::string sample(const char* filename) {
    return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/deluxe/" + filename;
}

bool exists(const std::string& p) {
    return std::filesystem::exists(p);
}

} // namespace

TEST_CASE("dpan: sniff + open + decode-first-frame on clean samples") {
    // The "clean" set — files without empty records, where our frame
    // count matches ffmpeg's exactly.
    struct sample_case {
        const char*  name;
        unsigned int width;
        unsigned int height;
        unsigned int min_frames;
    };
    constexpr sample_case cases[] = {
        {"SW.ANM",         320, 200, 43},
        {"Vrs.anm",        320, 200, 25},
        {"LIGHT3D.001",    320, 200, 50},
        {"abydos.lpf.anim",320, 200, 16},
    };

    auto& reg = onyx_anim::codec_registry::instance();

    for (const auto& c : cases) {
        const auto path = sample(c.name);
        if (!exists(path)) {
            MESSAGE("skipping DPaint sample (missing): " << c.name);
            continue;
        }

        auto stream = musac::io_from_file(path.c_str(), "rb");
        REQUIRE_MESSAGE(stream, "could not open ", c.name);

        auto dec = reg.create_decoder(stream.get());
        REQUIRE_MESSAGE(dec, "no codec sniffed ", c.name);
        CHECK(dec->name() == "dpan");

        REQUIRE(dec->open(stream.get()));
        const auto& info = dec->info();
        CHECK(info.width  == c.width);
        CHECK(info.height == c.height);
        CHECK(info.frame_count == c.min_frames);
        CHECK(info.frame_period.count() > 0);

        onyx_image::memory_surface surf;
        auto fr = dec->decode_frame(surf);
        REQUIRE(fr);
        CHECK(static_cast<unsigned int>(surf.width())  == c.width);
        CHECK(static_cast<unsigned int>(surf.height()) == c.height);
    }
}

TEST_CASE("dpan: HORSE.ANM with non-standard compression byte is accepted") {
    // HORSE.ANM advertises compression_type = 0 instead of the documented
    // value 1 — a non-standard authoring choice. Records still start with
    // the RunSkipDump 0x42 IDnum and decode correctly. ffmpeg rejects it
    // outright; we accept it.
    const auto path = sample("HORSE.ANM");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream);
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));
    CHECK(dec->info().frame_count == 32u);

    onyx_image::memory_surface surf;
    auto fr = dec->decode_frame(surf);
    REQUIRE(fr);
    CHECK(static_cast<unsigned int>(surf.width())  == 320u);
    CHECK(static_cast<unsigned int>(surf.height()) == 200u);
}

TEST_CASE("dpan: out-of-record-order pages are reassembled correctly") {
    // BALLMONS.ANM's page table is scrambled (page 0 holds frames 0-11,
    // page 1 holds 23-124, page 2 holds 12-22, etc.). The codec needs to
    // sort records by frame index, not by page-walk order.
    const auto path = sample("BALLMONS.ANM");
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
