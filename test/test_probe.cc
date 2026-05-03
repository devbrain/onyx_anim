#include <doctest/doctest.h>

#include <onyx_anim/sdk/probe.hh>
#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <musac/sdk/io_stream.hh>

#include <filesystem>
#include <string>

namespace {
    std::string sample(const char* dir, const char* fn) {
        return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/" + dir + "/" + fn;
    }
    std::string smk_sample(const char* fn) { return sample("smacker", fn); }
    bool exists(const std::string& p) { return std::filesystem::exists(p); }
}

TEST_CASE("probe: rejects null stream") {
    auto r = onyx_anim::probe(nullptr);
    CHECK_FALSE(r.has_value());
}

TEST_CASE("probe: returns codec, info, and audio tracks for a smk file") {
    const auto path = smk_sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream != nullptr);

    auto r = onyx_anim::probe(stream.get());
    REQUIRE(r.has_value());

    CHECK(r->codec_name == "smk");
    CHECK(r->video.width  > 0);
    CHECK(r->video.height > 0);
    CHECK(r->video.frame_count > 0);
    REQUIRE(r->audio_tracks.size() >= 1);
    CHECK(r->audio_tracks[0].sample_rate > 0);
    CHECK(r->audio_tracks[0].channels > 0);
}

TEST_CASE("probe: identifies a Bink file and reports DCT/RDFT codec name") {
    const auto path = sample("bink", "logo_lucas.bik");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream != nullptr);

    auto r = onyx_anim::probe(stream.get());
    REQUIRE(r.has_value());
    CHECK(r->codec_name == "bink");
    CHECK(r->video.width  > 0);
    CHECK(r->video.height > 0);
    if (!r->audio_tracks.empty()) {
        const std::string c = r->audio_tracks[0].codec_name;
        CHECK((c == "Bink Audio (DCT)" || c == "Bink Audio (RDFT)"));
    }
}

TEST_CASE("probe: identifies an FLC file and reports zero audio tracks") {
    const auto path = sample("fli-flc", "2422.FLC");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream != nullptr);

    auto r = onyx_anim::probe(stream.get());
    REQUIRE(r.has_value());
    CHECK(r->codec_name == "flc");
    CHECK(r->video.width  > 0);
    CHECK(r->video.height > 0);
    CHECK(r->audio_tracks.empty());
}

TEST_CASE("probe: identifies a CDXL file") {
    const auto path = sample("cdxl", "Discovery.CDXL");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream != nullptr);

    auto r = onyx_anim::probe(stream.get());
    REQUIRE(r.has_value());
    CHECK(r->codec_name == "cdxl");
    CHECK(r->video.width  > 0);
    CHECK(r->video.height > 0);
}

TEST_CASE("probe: identifies an Amiga ANIM file") {
    const auto path = sample("amiga-anim", "anim5_8bpp.anim");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream != nullptr);

    auto r = onyx_anim::probe(stream.get());
    REQUIRE(r.has_value());
    CHECK(r->codec_name == "amiga_anim");
    CHECK(r->video.width  > 0);
    CHECK(r->video.height > 0);
}

TEST_CASE("probe: leaves the stream cursor at its starting position") {
    const auto path = smk_sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream != nullptr);
    const auto pos_before = stream->tell();

    auto r = onyx_anim::probe(stream.get());
    REQUIRE(r.has_value());

    CHECK(stream->tell() == pos_before);

    // Stream is still usable — a fresh open() on the same stream succeeds.
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec != nullptr);
    auto open_r = dec->open(stream.get());
    CHECK(open_r.has_value());
}
