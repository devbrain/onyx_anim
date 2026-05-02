#include <doctest/doctest.h>

#include <flc/chunks.hh>

#include <array>
#include <cstdint>
#include <cstring>

TEST_CASE("parse_sub_chunk_header reads size and type") {
    constexpr std::array<std::uint8_t, 6> data = {
        0x12, 0x00, 0x00, 0x00,   // size = 18
        0x0F, 0x00,               // type = BRUN (15)
    };
    auto h = flc::parse_sub_chunk_header(data);
    REQUIRE(h);
    CHECK(h->size == 18);
    CHECK(h->type == flc::sub_chunk_type::brun);
}

TEST_CASE("parse_sub_chunk_header rejects truncated input") {
    std::array<std::uint8_t, 4> short_data{};
    auto h = flc::parse_sub_chunk_header(short_data);
    CHECK_FALSE(h);
}

TEST_CASE("parse_sub_chunk_header rejects size smaller than header") {
    constexpr std::array<std::uint8_t, 6> data = {
        0x03, 0x00, 0x00, 0x00,   // size = 3 (less than 6-byte header)
        0x0F, 0x00,
    };
    auto h = flc::parse_sub_chunk_header(data);
    CHECK_FALSE(h);
}

TEST_CASE("sub_chunk_payload returns trimmed span") {
    std::array<std::uint8_t, 10> data{};
    data[0] = 0x0A; // size = 10
    data[4] = 0x10; // type = COPY (16)
    for (std::size_t i = 6; i < 10; ++i) data[i] = static_cast<std::uint8_t>(i);

    auto p = flc::sub_chunk_payload(data);
    REQUIRE(p);
    CHECK(p->size() == 4);
    CHECK((*p)[0] == 6);
    CHECK((*p)[3] == 9);
}

TEST_CASE("sub_chunk_payload detects truncation") {
    std::array<std::uint8_t, 8> data{};
    data[0] = 0x10; // size = 16, but data is only 8 bytes
    data[4] = 0x10;
    auto p = flc::sub_chunk_payload(data);
    CHECK_FALSE(p);
}
