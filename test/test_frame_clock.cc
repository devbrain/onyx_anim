#include <doctest/doctest.h>

#include <onyx_anim/sdk/frame_clock.hh>

using onyx_anim::frame_clock;
using namespace std::chrono_literals;

TEST_CASE("frame_clock: realtime mode accumulates wall_dt") {
    frame_clock c;
    REQUIRE(c.current_mode() == frame_clock::mode::realtime);
    CHECK(c.now() == 0us);

    c.tick(100us);
    CHECK(c.now() == 100us);

    c.tick(50us);
    CHECK(c.now() == 150us);
}

TEST_CASE("frame_clock: pause freezes realtime advancement, resume continues") {
    frame_clock c;
    c.tick(100us);
    c.pause();
    c.tick(500us);
    CHECK(c.now() == 100us);   // tick during pause is a no-op

    c.resume();
    c.tick(50us);
    CHECK(c.now() == 150us);
}

TEST_CASE("frame_clock: seek_to sets the current time in any mode") {
    frame_clock c;
    c.tick(1000us);
    c.seek_to(0us);
    CHECK(c.now() == 0us);

    c.use_external_clock();
    c.seek_to(500us);
    CHECK(c.now() == 500us);
}

TEST_CASE("frame_clock: external mode is driven only by set_time") {
    frame_clock c;
    c.use_external_clock();
    CHECK(c.current_mode() == frame_clock::mode::external);
    CHECK(c.now() == 0us);

    c.set_time(1234us);
    CHECK(c.now() == 1234us);

    // tick is a no-op in external mode
    c.tick(99us);
    CHECK(c.now() == 1234us);

    c.set_time(500us);
    CHECK(c.now() == 500us);
}

TEST_CASE("frame_clock: audio mode polls the supplied query function") {
    frame_clock c;
    std::chrono::microseconds simulated_audio_pos{0};
    c.use_audio_clock([&]() { return simulated_audio_pos; });
    CHECK(c.current_mode() == frame_clock::mode::audio);
    CHECK(c.now() == 0us);

    simulated_audio_pos = 500us;
    CHECK(c.now() == 500us);

    simulated_audio_pos = 12345us;
    CHECK(c.now() == 12345us);
}

TEST_CASE("frame_clock: audio mode disengaged falls back to realtime preserving position") {
    frame_clock c;
    std::chrono::microseconds simulated_audio_pos{1000us};
    c.use_audio_clock([&]() { return simulated_audio_pos; });
    CHECK(c.now() == 1000us);

    // Engine drops the audio_stream → caller passes nullptr to detach.
    c.use_audio_clock({});
    CHECK(c.current_mode() == frame_clock::mode::realtime);
    CHECK(c.now() == 1000us);   // didn't jump backwards

    c.tick(100us);
    CHECK(c.now() == 1100us);
}

TEST_CASE("frame_clock: switching modes preserves position") {
    frame_clock c;
    c.tick(2000us);
    c.use_external_clock();
    CHECK(c.now() == 2000us);

    c.use_realtime_clock();
    CHECK(c.now() == 2000us);
    c.tick(100us);
    CHECK(c.now() == 2100us);
}

TEST_CASE("frame_clock: rewind() resets to zero") {
    frame_clock c;
    c.tick(5000us);
    c.rewind();
    CHECK(c.now() == 0us);
}

TEST_CASE("frame_clock: negative wall_dt is ignored (defensive)") {
    frame_clock c;
    c.tick(100us);
    c.tick(-50us);
    CHECK(c.now() == 100us);
}
