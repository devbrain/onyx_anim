#include <doctest/doctest.h>

#include <onyx_anim/player/player.hh>
#include <onyx_anim/sdk/codec_registry.hh>

#include <onyx_image/surface.hpp>

#include <musac/sdk/io_stream.hh>

#include <filesystem>
#include <string>

namespace {
    std::string smk_sample(const char* filename) {
        return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/smacker/" + filename;
    }

    bool exists(const std::string& p) {
        return std::filesystem::exists(p);
    }
} // namespace

TEST_CASE("player: rejects null io_stream") {
    auto r = onyx_anim::player::open(nullptr);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("player: opens a smk file, reports sane info, ticks frames sequentially") {
    const auto path = smk_sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream != nullptr);

    auto pr = onyx_anim::player::open(std::move(stream));
    REQUIRE(pr.has_value());
    auto p = std::move(*pr);

    const auto& info = p->info();
    CHECK(info.width > 0);
    CHECK(info.height > 0);
    CHECK(info.frame_count > 0);
    CHECK(info.frame_period.count() > 0);

    // Without an audio_device, the clock is in realtime mode. Drive it
    // forward one frame_period at a time, decoding consecutive frames.
    p->play();
    CHECK(p->is_playing());

    onyx_image::memory_surface out;

    // Tick once at t=0 — should produce frame 0.
    bool got = p->advance_to_time(std::chrono::microseconds{0}, out);
    CHECK(got);

    // Advance to the next frame's timestamp.
    got = p->advance_to_time(info.frame_period, out);
    CHECK(got);

    // Calling advance with the same timestamp again should be a no-op.
    got = p->advance_to_time(info.frame_period, out);
    CHECK_FALSE(got);
}

TEST_CASE("player: pause prevents ticking") {
    const auto path = smk_sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    auto pr = onyx_anim::player::open(std::move(stream));
    REQUIRE(pr.has_value());
    auto p = std::move(*pr);

    p->play();
    p->pause();
    CHECK_FALSE(p->is_playing());

    onyx_image::memory_surface out;
    CHECK_FALSE(p->advance_to_time(std::chrono::microseconds{0}, out));
}

TEST_CASE("player: take_audio_track delivers the source when no audio_device was supplied") {
    const auto path = smk_sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    auto pr = onyx_anim::player::open(std::move(stream));
    REQUIRE(pr.has_value());
    auto p = std::move(*pr);

    REQUIRE(p->audio_track_count() > 0);
    auto src = p->take_audio_track(0);
    CHECK(src != nullptr);
}
