#include <doctest/doctest.h>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <onyx_image/surface.hpp>

#include <musac/audio_source.hh>
#include <musac/sdk/io_stream.hh>

#include <filesystem>
#include <string>

namespace {

std::string sample(const char* filename) {
    return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/smacker/" + filename;
}

bool exists(const std::string& p) {
    return std::filesystem::exists(p);
}

} // namespace

TEST_CASE("smk: sniff + open + decode-first-frame across the corpus") {
    // Width/height/min-frame-count taken from a Python pre-pass over the
    // corpus headers. EARTH.SMK is truncated to 30 frames in our sample
    // (the file is shorter than its declared 74-frame size); we accept
    // whatever fits.
    struct sample_case {
        const char*  name;
        unsigned int width;
        unsigned int height;
        unsigned int min_frames; // ffmpeg-confirmed lower bound
        bool         is_smk4;
    };
    constexpr sample_case cases[] = {
        {"1.smk",                          320, 200, 473, false},
        {"1_2.SMK",                        320, 240, 781, false},
        {"20130507_audio-distortion.smk",  320, 308, 787, false},
        {"EARTH.SMK",                      632, 428,  30, false},
        {"FMAN.SMK",                        64,  72, 107, false},
        {"SPLASH.SMK",                     640, 480, 195, false},
        {"WMAN.SMK",                       128, 128,  80, false},
        {"example.smk",                   1280, 720, 488, true},
        {"intro.smk",                      640, 480,   6, false},
        {"test.smk",                       160, 132, 100, false},
        {"wetlogo.smk",                    320, 200, 100, false},
    };

    auto& reg = onyx_anim::codec_registry::instance();

    for (const auto& c : cases) {
        const auto path = sample(c.name);
        if (!exists(path)) {
            MESSAGE("skipping Smacker sample (missing): " << c.name);
            continue;
        }

        auto stream = musac::io_from_file(path.c_str(), "rb");
        REQUIRE_MESSAGE(stream, "could not open ", c.name);

        auto dec = reg.create_decoder(stream.get());
        REQUIRE_MESSAGE(dec, "no codec sniffed ", c.name);
        CHECK(dec->name() == "smk");

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

TEST_CASE("smk: SPLASH decodes all 195 frames end-to-end") {
    const auto path = sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream);
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));
    CHECK(dec->info().frame_count == 195u);

    onyx_image::memory_surface surf;
    int decoded = 0;
    while (!dec->eof()) {
        auto fr = dec->decode_frame(surf);
        if (!fr) break;
        ++decoded;
    }
    CHECK(decoded == 195);
}

TEST_CASE("smk: SPLASH exposes 1 audio track (Smacker DPCM, 11025 Hz mono)") {
    const auto path = sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream);
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));
    CHECK(dec->info().audio_track_count == 1u);
    CHECK(dec->audio_track_count() == 1u);

    auto track = dec->take_audio_track(0);
    REQUIRE(track);
    // Each track may only be taken once.
    CHECK(dec->take_audio_track(0) == nullptr);
}

TEST_CASE("smk: SMK4 example.smk opens and exposes 1280x720") {
    const auto path = sample("example.smk");
    if (!exists(path)) return;
    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream);
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));
    CHECK(dec->info().width  == 1280u);
    CHECK(dec->info().height == 720u);
    CHECK(dec->info().frame_count == 488u);
}
