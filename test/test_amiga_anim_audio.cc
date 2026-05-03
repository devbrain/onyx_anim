#include <doctest/doctest.h>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <onyx_image/surface.hpp>

#include <musac/sdk/io_stream.hh>
#include <musac/sdk/decoder.hh>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// Path to the canonical AnimFX-audio sample. Tests are skipped if it isn't
// present so the suite keeps working in source-only checkouts.
const char* sndanim_path() {
    // Same convention as the FFmpeg cross-checks: data lives in the source
    // tree alongside data/amiga-anim/.
    static const std::string p =
        std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) +
        "/amiga-anim/Wz.sndanim";
    return p.c_str();
}

bool sndanim_available() {
    return std::filesystem::exists(sndanim_path());
}

const char* sla_path() {
    static const std::string p =
        std::string(NEUTRINO_ONYX_ANIM_DATA_DIR) +
        "/amiga-anim/Gressklippermannen";
    return p.c_str();
}

bool sla_available() {
    return std::filesystem::exists(sla_path());
}

} // namespace

TEST_CASE("amiga_anim: AnimFX audio track is exposed") {
    if (!sndanim_available()) {
        MESSAGE("Wz.sndanim missing; skipping AnimFX audio integration test");
        return;
    }

    // test_main.cc already registers all codecs at startup; re-registering
    // here would duplicate the factory list.
    auto& reg = onyx_anim::codec_registry::instance();

    auto stream = musac::io_from_file(sndanim_path(), "rb");
    REQUIRE(stream);

    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    const auto& info = dec->info();
    CHECK(info.audio_track_count == 1u);
    CHECK(info.audio_rate     == 13017u);   // SXHD.PlayFreq
    CHECK(info.audio_channels == 2u);       // SXHD.UsedMode = stereo

    auto track = dec->take_audio_track(0u);
    REQUIRE(track);
    CHECK(track->get_rate()     == 13017u);
    CHECK(track->get_channels() == 2u);

    // Per the file: 109 SBDY chunks, 282 804 bytes total, stereo 8-bit @ 13017 Hz.
    // After L/R interleave the PCM-frame count is bytes / 2 = 141 402.
    // Duration ≈ 141 402 / 13017 ≈ 10.863 s.
    const auto dur = track->duration();
    CHECK(dur >= std::chrono::milliseconds(10800));
    CHECK(dur <= std::chrono::milliseconds(10900));

    // Each track is one-shot; second take returns nullptr.
    CHECK(dec->take_audio_track(0u) == nullptr);
}

TEST_CASE("amiga_anim: AnimFX audio decode produces normalized samples") {
    if (!sndanim_available()) return;

    // test_main.cc already registers all codecs at startup; re-registering
    // here would duplicate the factory list.
    auto& reg = onyx_anim::codec_registry::instance();

    auto stream = musac::io_from_file(sndanim_path(), "rb");
    REQUIRE(stream);
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    auto track = dec->take_audio_track(0u);
    REQUIRE(track);

    // Pull the first 1024 stereo sample-frames (= 2048 floats) and check
    // that nothing is out of [-1, 1] and at least one sample is non-zero.
    std::vector<float> buf(2048u, 0.0f);
    bool call_again = false;
    const std::size_t got =
        track->decode(buf.data(), buf.size(), call_again,
                      static_cast<musac::channels_t>(2));
    REQUIRE(got > 0u);
    bool any_nonzero = false;
    for (std::size_t i = 0; i < got; ++i) {
        CHECK(buf[i] >= -1.0f);
        CHECK(buf[i] <=  1.0f);
        if (buf[i] != 0.0f) any_nonzero = true;
    }
    CHECK(any_nonzero);
}

TEST_CASE("amiga_anim: ANIM+SLA exposes per-frame audio events") {
    if (!sla_available()) {
        MESSAGE("Gressklippermannen missing; skipping ANIM+SLA test");
        return;
    }

    auto& reg = onyx_anim::codec_registry::instance();
    auto stream = musac::io_from_file(sla_path(), "rb");
    REQUIRE(stream);
    auto dec = reg.create_decoder(stream.get());
    REQUIRE(dec);
    REQUIRE(dec->open(stream.get()));

    // ANIM+SLA has no SXHD/SBDY, so the continuous-track count is 0; events
    // are exposed via pending_audio_events() instead.
    CHECK(dec->info().audio_track_count == 0u);
    CHECK(dec->take_audio_track(0u) == nullptr);

    onyx_image::memory_surface surf;
    int total_events = 0;
    int frames_with_events = 0;
    bool saw_valid_8svx_header = false;

    while (!dec->eof()) {
        auto fr = dec->decode_frame(surf);
        if (!fr) break;
        const auto evs = dec->pending_audio_events();
        if (!evs.empty()) {
            ++frames_with_events;
            total_events += static_cast<int>(evs.size());
            for (const auto& e : evs) {
                REQUIRE(e.sound_bytes);
                REQUIRE(e.sound_bytes->size() >= 12u);
                // First 4 bytes must be "FORM", inner type "8SVX".
                const auto& b = *e.sound_bytes;
                if (b[0]=='F' && b[1]=='O' && b[2]=='R' && b[3]=='M' &&
                    b[8]=='8' && b[9]=='S' && b[10]=='V' && b[11]=='X') {
                    saw_valid_8svx_header = true;
                }
                CHECK(e.volume >= 0.0f);
                CHECK(e.volume <= 1.0f);
            }
        }
    }
    CHECK(total_events > 0);
    CHECK(frames_with_events > 0);
    CHECK(saw_valid_8svx_header);
}
