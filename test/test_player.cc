#include <doctest/doctest.h>

#include <onyx_anim/player/player.hh>
#include <onyx_anim/sdk/codec_registry.hh>

#include <onyx_image/surface.hpp>

#include <musac/audio_source.hh>
#include <musac/sdk/io_stream.hh>

#include <filesystem>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {
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

    std::vector<std::uint8_t> build_tick_test_flc() {
        constexpr std::uint16_t W = 2;
        constexpr std::uint16_t H = 2;
        constexpr std::uint16_t frame_count = 100;
        constexpr std::uint32_t frame_size = 16;
        constexpr std::uint32_t file_size = 128 + frame_size * frame_count;

        std::vector<std::uint8_t> out;
        out.reserve(file_size);

        put_u32le(out, file_size);
        put_u16le(out, 0xAF12);
        put_u16le(out, frame_count);
        put_u16le(out, W); put_u16le(out, H);
        put_u16le(out, 8); put_u16le(out, 3);
        put_u32le(out, 1);                         // 1 ms/frame
        put_u16le(out, 0);
        put_u32le(out, 0); put_u32le(out, 0); put_u32le(out, 0); put_u32le(out, 0);
        put_u16le(out, 1); put_u16le(out, 1);
        put_u16le(out, 0); put_u16le(out, 0); put_u16le(out, frame_count);
        put_u32le(out, 0);
        put_u16le(out, 0); put_u16le(out, 0);
        out.insert(out.end(), 24, 0);
        put_u32le(out, 128);
        put_u32le(out, 128 + frame_size);
        out.insert(out.end(), 40, 0);
        REQUIRE(out.size() == 128);

        for (std::uint16_t i = 0; i < frame_count; ++i) {
            put_u32le(out, frame_size);
            put_u16le(out, 0xF1FA);
            put_u16le(out, 0);                     // no subchunks
            put_u16le(out, 0); put_u16le(out, 0); put_u16le(out, 0); put_u16le(out, 0);
        }

        REQUIRE(out.size() == file_size);
        return out;
    }

    std::string smk_sample(const char* filename) {
        return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/smacker/" + filename;
    }

    std::string flc_sample(const char* filename) {
        return std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) + "/fli-flc/" + filename;
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

TEST_CASE("player: opens an FLI file and renders the first frame") {
    const auto path = flc_sample("test2.fli");
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream != nullptr);

    auto pr = onyx_anim::player::open(std::move(stream));
    REQUIRE(pr.has_value());
    auto p = std::move(*pr);

    p->play();

    onyx_image::memory_surface out;
    CHECK(p->advance_to_time(std::chrono::microseconds{0}, out));
    CHECK(out.width() == static_cast<int>(p->info().width));
    CHECK(out.height() == static_cast<int>(p->info().height));
}

TEST_CASE("player: tick advances the realtime clock") {
    const auto bytes = build_tick_test_flc();
    auto stream = musac::io_from_memory(bytes.data(), bytes.size());
    REQUIRE(stream != nullptr);

    auto pr = onyx_anim::player::open(std::move(stream));
    REQUIRE(pr.has_value());
    auto p = std::move(*pr);

    p->play();

    onyx_image::memory_surface out;
    CHECK(p->tick(out));
    const auto first_time = p->current_time();

    std::this_thread::sleep_for(std::chrono::milliseconds{5});

    CHECK(p->tick(out));
    CHECK(p->current_time() > first_time);
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

TEST_CASE("player: opens an ANIM+SLA file with no streaming track and exposes per-frame events") {
    const auto path = std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) +
                      "/amiga-anim/Gressklippermannen";
    if (!exists(path)) return;

    auto stream = musac::io_from_file(path.c_str(), "rb");
    REQUIRE(stream != nullptr);

    auto pr = onyx_anim::player::open(std::move(stream));
    REQUIRE(pr.has_value());
    auto p = std::move(*pr);

    // ANIM+SLA exposes audio only via per-frame events, not as a
    // streaming track.
    CHECK(p->audio_track_count() == 0u);

    p->play();
    onyx_image::memory_surface out;

    // Walk a handful of frames and collect any events the codec
    // surfaces. SCTL triggers may not land on the very first frame
    // depending on the file, so probe a window.
    bool saw_event = false;
    const auto period = p->info().frame_period;
    for (unsigned int i = 0; i < 16u; ++i) {
        p->advance_to_time(period * i, out);
        if (!p->pending_audio_events().empty()) {
            saw_event = true;
            break;
        }
    }
    CHECK(saw_event);
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
