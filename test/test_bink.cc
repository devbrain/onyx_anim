#include <doctest/doctest.h>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <onyx_image/surface.hpp>

#include <musac/audio_source.hh>
#include <musac/sdk/io_stream.hh>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace {

std::string sample(const char* filename) {
    return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/bink/" + filename;
}

bool exists(const std::string& p) {
    return std::filesystem::exists(p);
}

} // namespace

TEST_CASE("bink: sniff + open + decode-first-frame across the corpus") {
    // (width, height, frame_count, fps_n/fps_d) verified against ffmpeg.
    // BIK[b] (DEFENDALL.BIK) and Bink2 (example.bk2) are intentionally
    // excluded — explicitly unsupported.
    struct sample_case {
        const char*  name;
        unsigned int width;
        unsigned int height;
        unsigned int frames;
    };
    constexpr sample_case cases[] = {
        {"end_victory.bik", 320, 200,  250},
        {"OpenPt1.bik",     640, 272,  170},
        {"logo_lucas.bik",  640, 480,  266},
        {"original.bik",    512, 384,  280},
        {"phar_intro.bik",  480, 143, 1470},
        {"intro.bik",       512, 384, 1204},
        {"example.bik",    1280, 720,  488},
    };

    auto& reg = onyx_anim::codec_registry::instance();

    for (const auto& c : cases) {
        const auto path = sample(c.name);
        if (!exists(path)) {
            MESSAGE("skipping Bink sample (missing): " << c.name);
            continue;
        }

        auto stream = musac::io_from_file(path.c_str(), "rb");
        REQUIRE_MESSAGE(stream, "could not open ", c.name);

        auto dec = reg.create_decoder(stream.get());
        REQUIRE_MESSAGE(dec, "no codec sniffed ", c.name);
        CHECK(dec->name() == "bink");

        REQUIRE(dec->open(stream.get()));
        const auto& info = dec->info();
        CHECK(info.width  == c.width);
        CHECK(info.height == c.height);
        CHECK(info.frame_count == c.frames);
        CHECK(info.frame_period.count() > 0);

        onyx_image::memory_surface surf;
        auto fr = dec->decode_frame(surf);
        REQUIRE(fr);
        CHECK(static_cast<unsigned int>(surf.width())  == c.width);
        CHECK(static_cast<unsigned int>(surf.height()) == c.height);
    }
}

TEST_CASE("bink: logo_lucas exposes its audio track") {
    // logo_lucas.bik has 1 stereo Bink Audio (DCT) track at 44 kHz.
    const auto path = sample("logo_lucas.bik");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream);
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));
    CHECK(dec->info().audio_track_count == 1u);

    auto track = dec->take_audio_track(0);
    REQUIRE(track);
    // Each track may only be taken once.
    CHECK(dec->take_audio_track(0) == nullptr);
}

TEST_CASE("bink: BIK[b] revision is rejected (not yet supported)") {
    const auto path = sample("DEFENDALL.BIK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream);
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    CHECK(dec->name() == "bink");
    // sniff matches, but open() should refuse with a clear error.
    CHECK_FALSE(dec->open(stream.get()));
}

TEST_CASE("bink: Bink2 magic (KB2*) would be rejected") {
    // Synthesise a 4-byte "KB2i" prefix — we don't keep a real Bink2
    // sample in the corpus (the only .bk2 file we have is actually a
    // mis-named BIKi file). The point of the test is to nail down the
    // sniff contract: bink decoder accepts BIK[bfghik] only.
    constexpr std::array<std::uint8_t, 4> kb2 = {'K', 'B', '2', 'i'};
    auto stream = musac::io_from_memory(kb2.data(), kb2.size());
    auto& reg = onyx_anim::codec_registry::instance();
    auto bink = reg.create_decoder("bink");
    REQUIRE(bink);
    CHECK_FALSE(bink->sniff(stream.get()));
}
