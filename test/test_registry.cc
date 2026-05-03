#include <doctest/doctest.h>

#include <onyx_anim/sdk/codec_registry.hh>

#include <musac/sdk/io_stream.hh>

#include <array>
#include <cstdint>

TEST_CASE("registry has codecs registered by test_main") {
    // test_main.cc registers FLC + SMK + atari_seq + amiga_anim + cdxl
    // at startup.
    auto& reg = onyx_anim::codec_registry::instance();
    CHECK(reg.factory_count() == 5);
}

TEST_CASE("registry creates flc decoder by name") {
    auto& reg = onyx_anim::codec_registry::instance();
    auto d = reg.create_decoder("flc");
    REQUIRE(d);
    CHECK(d->name() == "flc");
}

TEST_CASE("registry creates smk decoder by name") {
    auto& reg = onyx_anim::codec_registry::instance();
    auto d = reg.create_decoder("smk");
    REQUIRE(d);
    CHECK(d->name() == "smk");
}

TEST_CASE("registry returns nullptr for unknown codec") {
    auto& reg = onyx_anim::codec_registry::instance();
    CHECK(reg.create_decoder("does-not-exist") == nullptr);
}

TEST_CASE("registry creates decoder from sniff") {
    const std::array<std::uint8_t, 4> smk = {'S', 'M', 'K', '2'};
    auto s = musac::io_from_memory(smk.data(), smk.size());

    auto& reg = onyx_anim::codec_registry::instance();
    auto d = reg.create_decoder(s.get());
    REQUIRE(d);
    CHECK(d->name() == "smk");
}
