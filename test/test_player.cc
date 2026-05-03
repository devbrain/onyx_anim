#include <doctest/doctest.h>

#include <onyx_anim/player/player.hh>
#include <onyx_anim/sdk/codec_registry.hh>

#include <onyx_image/surface.hpp>

#include <musac/audio_source.hh>
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

TEST_CASE("player: take_audio_track exposes a usable io_stream observer") {
    const auto path = smk_sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    auto pr = onyx_anim::player::open(std::move(stream));
    REQUIRE(pr.has_value());
    auto p = std::move(*pr);

    musac::io_stream* io = nullptr;
    auto src = p->take_audio_track(0, &io);
    REQUIRE(src);
    REQUIRE(io != nullptr);

    // Fresh source: cursor at byte 0.
    CHECK(io->tell() == 0);
}

TEST_CASE("player: seek_to_time updates current_time and re-decodes that frame") {
    const auto path = smk_sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    auto pr = onyx_anim::player::open(std::move(stream));
    REQUIRE(pr.has_value());
    auto p = std::move(*pr);
    p->play();

    const auto period = p->info().frame_period;
    const auto target = period * 10;

    p->seek_to_time(target);
    CHECK(p->current_time() == target);

    onyx_image::memory_surface out;
    CHECK(p->advance_to_time(target, out));
}

TEST_CASE("player: rewind() resets current_time to zero") {
    const auto path = smk_sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    auto pr = onyx_anim::player::open(std::move(stream));
    REQUIRE(pr.has_value());
    auto p = std::move(*pr);
    p->play();

    const auto period = p->info().frame_period;
    p->seek_to_time(period * 5);
    REQUIRE(p->current_time() == period * 5);

    p->rewind();
    CHECK(p->current_time() == std::chrono::microseconds{0});

    onyx_image::memory_surface out;
    CHECK(p->advance_to_time(std::chrono::microseconds{0}, out));
}

TEST_CASE("player: end-of-stream callback fires when last frame is past, no loop") {
    const auto path = smk_sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    onyx_anim::player_options opts;
    opts.loop = false;
    auto pr = onyx_anim::player::open(std::move(stream), opts);
    REQUIRE(pr.has_value());
    auto p = std::move(*pr);

    bool eos_fired = false;
    p->on_end_of_stream([&] { eos_fired = true; });
    p->play();

    const auto& info = p->info();
    const auto past_end = info.frame_period * (info.frame_count + 1);

    onyx_image::memory_surface out;
    p->advance_to_time(past_end, out);

    CHECK(eos_fired);
    CHECK(p->eof());
    CHECK_FALSE(p->is_playing());
}

TEST_CASE("player: looping wraps around at duration without firing eos") {
    const auto path = smk_sample("SPLASH.SMK");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    onyx_anim::player_options opts;
    opts.loop = true;
    auto pr = onyx_anim::player::open(std::move(stream), opts);
    REQUIRE(pr.has_value());
    auto p = std::move(*pr);

    bool eos_fired = false;
    p->on_end_of_stream([&] { eos_fired = true; });
    p->play();

    const auto& info = p->info();
    REQUIRE(info.duration.count() > 0);

    onyx_image::memory_surface out;
    // Drive past the end — looping should re-decode early frames.
    const auto past_end = info.duration + info.frame_period * 3;
    p->advance_to_time(past_end, out);

    CHECK_FALSE(eos_fired);
    CHECK_FALSE(p->eof());
    CHECK(p->is_playing());
}
