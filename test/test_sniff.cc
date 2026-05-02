#include <doctest/doctest.h>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <musac/sdk/io_stream.hh>

#include <array>
#include <cstdint>

namespace {
    // Minimal FLC header: 4 bytes size + 2 bytes magic 0xAF12.
    const std::array<std::uint8_t, 6> kFlcMagic = {0, 0, 0, 0, 0x12, 0xAF};
    const std::array<std::uint8_t, 6> kFliMagic = {0, 0, 0, 0, 0x11, 0xAF};

    const std::array<std::uint8_t, 4> kSmk2Magic = {'S', 'M', 'K', '2'};
    const std::array<std::uint8_t, 4> kSmk4Magic = {'S', 'M', 'K', '4'};

    const std::array<std::uint8_t, 6> kGarbage = {0, 0, 0, 0, 0xFF, 0xFF};
} // namespace

TEST_CASE("FLC magic auto-detects as flc") {
    auto s = musac::io_from_memory(kFlcMagic.data(), kFlcMagic.size());
    auto dec = onyx_anim::codec_registry::instance().create_decoder(s.get());
    REQUIRE(dec);
    CHECK(dec->name() == "flc");
    // Sniffers must restore stream position.
    CHECK(s->tell() == 0);
}

TEST_CASE("FLI magic auto-detects as flc") {
    auto s = musac::io_from_memory(kFliMagic.data(), kFliMagic.size());
    auto dec = onyx_anim::codec_registry::instance().create_decoder(s.get());
    REQUIRE(dec);
    CHECK(dec->name() == "flc");
}

TEST_CASE("flc decoder rejects garbage") {
    auto dec = onyx_anim::codec_registry::instance().create_decoder("flc");
    REQUIRE(dec);
    auto s = musac::io_from_memory(kGarbage.data(), kGarbage.size());
    CHECK_FALSE(dec->sniff(s.get()));
}

TEST_CASE("SMK2 magic auto-detects as smk") {
    auto s = musac::io_from_memory(kSmk2Magic.data(), kSmk2Magic.size());
    auto dec = onyx_anim::codec_registry::instance().create_decoder(s.get());
    REQUIRE(dec);
    CHECK(dec->name() == "smk");
    CHECK(s->tell() == 0);
}

TEST_CASE("SMK4 magic auto-detects as smk") {
    auto s = musac::io_from_memory(kSmk4Magic.data(), kSmk4Magic.size());
    auto dec = onyx_anim::codec_registry::instance().create_decoder(s.get());
    REQUIRE(dec);
    CHECK(dec->name() == "smk");
}

TEST_CASE("smk decoder rejects FLC magic") {
    auto dec = onyx_anim::codec_registry::instance().create_decoder("smk");
    REQUIRE(dec);
    auto s = musac::io_from_memory(kFlcMagic.data(), kFlcMagic.size());
    CHECK_FALSE(dec->sniff(s.get()));
}
