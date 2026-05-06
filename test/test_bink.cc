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
#include <string_view>
#include <vector>

namespace {

std::string sample(const char* filename) {
    return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/bink/" + filename;
}

bool exists(const std::string& p) {
    return std::filesystem::exists(p);
}

void put_u16le(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
}

void put_u32le(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFFu));
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

    // Per-track metadata exposed via audio_track(idx).
    const auto t = dec->audio_track(0);
    CHECK(t.sample_rate == 44000u);
    CHECK(t.channels    == 2u);
    CHECK(std::string_view{t.codec_name} == "Bink Audio (DCT)");
    CHECK(t.duration.count() > 0);

    // Out-of-range track index → zeroed struct.
    const auto bad = dec->audio_track(99);
    CHECK(bad.sample_rate == 0u);
    CHECK(std::string_view{bad.codec_name}.empty());

    auto track = dec->take_audio_track(0);
    REQUIRE(track);
    // Each track may only be taken once.
    CHECK(dec->take_audio_track(0) == nullptr);
}

TEST_CASE("bink: phar_intro reports RDFT codec name") {
    const auto path = sample("phar_intro.bik");
    if (!exists(path)) return;
    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream);
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));
    REQUIRE(dec->audio_track_count() == 1u);
    const auto t = dec->audio_track(0);
    CHECK(std::string_view{t.codec_name} == "Bink Audio (RDFT)");
    CHECK(t.sample_rate == 44100u);
    CHECK(t.channels    == 2u);
}

TEST_CASE("bink: BIK[b] revision opens + decodes (DEFENDALL.BIK)") {
    // BIK[b] uses the older bundle layout (10 bundles, fixed bit-field
    // widths, no per-frame Huffman trees) and a different block-type
    // vocabulary than modern Bink. The decoder is wired but the chroma
    // path still has a residual bug — Y plane is mostly correct, U/V
    // diverge significantly. We assert open + first-frame decode but
    // don't include this file in the strict cross-check ctest list.
    const auto path = sample("DEFENDALL.BIK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream);
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    CHECK(dec->name() == "bink");
    REQUIRE(dec->open(stream.get()));
    CHECK(dec->info().width  == 256u);
    CHECK(dec->info().height == 120u);
    CHECK(dec->info().frame_count == 99u);

    onyx_image::memory_surface surf;
    auto fr = dec->decode_frame(surf);
    REQUIRE(fr);
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

TEST_CASE("bink: oversized audio chunk is rejected without integer wrap") {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(68);

    bytes.insert(bytes.end(), {'B', 'I', 'K', 'i'});
    put_u32le(bytes, 60);             // file size minus 8
    put_u32le(bytes, 1);              // frames
    put_u32le(bytes, 4);              // largest frame
    put_u32le(bytes, 1);              // duration
    put_u32le(bytes, 8);              // width
    put_u32le(bytes, 8);              // height
    put_u32le(bytes, 15);             // fps numerator
    put_u32le(bytes, 1);              // fps denominator
    put_u32le(bytes, 0);              // flags
    put_u32le(bytes, 1);              // audio tracks

    put_u32le(bytes, 1024);           // max decoded audio bytes
    put_u16le(bytes, 22050);          // sample rate
    put_u16le(bytes, 0);              // RDFT mono
    put_u32le(bytes, 0);              // track id

    put_u32le(bytes, 65);             // frame offset 64, keyframe bit set
    put_u32le(bytes, 68);             // frame end
    put_u32le(bytes, 0xFFFFFFFFu);    // impossible audio payload size

    auto stream = musac::io_from_memory(bytes.data(), bytes.size());
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder("bink");
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    onyx_image::memory_surface surf;
    CHECK_FALSE(dec->decode_frame(surf));
}
