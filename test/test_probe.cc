#include <doctest/doctest.h>

#include <onyx_anim/sdk/probe.hh>
#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <musac/sdk/io_stream.hh>

#include <filesystem>
#include <string>

namespace {
    std::string smk_sample(const char* fn) {
        return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/smacker/" + fn;
    }
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
