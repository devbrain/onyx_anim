#include <doctest/doctest.h>

#include <bytes/bytes.hh>

#include <array>
#include <cstdint>

TEST_CASE("free pointer readers") {
    constexpr std::array<std::uint8_t, 4> b = {0x78, 0x56, 0x34, 0x12};
    CHECK(bytes::read_u8(b.data())   == 0x78);
    CHECK(bytes::read_u16le(b.data()) == 0x5678);
    CHECK(bytes::read_u16be(b.data()) == 0x7856);
    CHECK(bytes::read_u32le(b.data()) == 0x12345678u);
    CHECK(bytes::read_u32be(b.data()) == 0x78563412u);
}

TEST_CASE("free pointer signed readers preserve sign") {
    constexpr std::array<std::uint8_t, 4> b = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(bytes::read_s16le(b.data()) == -1);
    CHECK(bytes::read_s32le(b.data()) == -1);
}

TEST_CASE("byte_reader reads a chain of fields") {
    constexpr std::array<std::uint8_t, 10> b = {
        0x01,                               // u8 = 1
        0x02, 0x00,                         // u16le = 2
        0x03, 0x00, 0x00, 0x00,             // u32le = 3
        0x00, 0xFF,                         // u16be = 0x00FF
    };
    bytes::byte_reader br{b};

    std::uint8_t  a = 0;
    std::uint16_t c = 0;
    std::uint32_t d = 0;
    std::uint16_t e = 0;
    br >> bytes::u8(a)
       >> bytes::u16le(c)
       >> bytes::u32le(d)
       >> bytes::u16be(e);

    CHECK(static_cast<bool>(br));
    CHECK_FALSE(br.failed());
    CHECK(a == 1);
    CHECK(c == 2);
    CHECK(d == 3u);
    CHECK(e == 0x00FFu);
    CHECK(br.position() == 9);
    CHECK(br.remaining() == 1);
}

TEST_CASE("byte_reader reading past end sets sticky failure") {
    constexpr std::array<std::uint8_t, 3> b = {0x01, 0x02, 0x03};
    bytes::byte_reader br{b};

    std::uint32_t v = 0;
    br >> bytes::u32le(v);
    CHECK_FALSE(static_cast<bool>(br));
    CHECK(br.failed());
    CHECK(v == 0);

    // After failure, further reads remain no-ops; cursor does not advance.
    std::uint8_t a = 0xAA;
    br >> bytes::u8(a);
    CHECK(br.failed());
    CHECK(a == 0);
}

TEST_CASE("byte_reader skip advances and detects past-end") {
    constexpr std::array<std::uint8_t, 4> b = {1, 2, 3, 4};
    bytes::byte_reader br{b};

    br >> bytes::skip(2);
    CHECK(br.position() == 2);
    CHECK(br);

    br >> bytes::skip(3);
    CHECK_FALSE(br);
}

TEST_CASE("byte_reader take returns sub-span and advances") {
    constexpr std::array<std::uint8_t, 6> b = {1, 2, 3, 4, 5, 6};
    bytes::byte_reader br{b};

    auto s = br.take(3);
    REQUIRE(s.size() == 3);
    CHECK(s[0] == 1);
    CHECK(s[2] == 3);
    CHECK(br.position() == 3);
}

TEST_CASE("byte_reader (LE default) reads u16/u32 as little-endian") {
    constexpr std::array<std::uint8_t, 6> b = {
        0x34, 0x12,
        0x78, 0x56, 0x34, 0x12,
    };
    bytes::byte_reader br{b};
    std::uint16_t a = 0;
    std::uint32_t c = 0;
    br >> bytes::u16(a) >> bytes::u32(c);
    CHECK(a == 0x1234);
    CHECK(c == 0x12345678u);
}

TEST_CASE("byte_reader_be reads u16/u32 as big-endian") {
    constexpr std::array<std::uint8_t, 6> b = {
        0x12, 0x34,
        0x12, 0x34, 0x56, 0x78,
    };
    bytes::byte_reader_be br{b};
    std::uint16_t a = 0;
    std::uint32_t c = 0;
    br >> bytes::u16(a) >> bytes::u32(c);
    CHECK(a == 0x1234);
    CHECK(c == 0x12345678u);
}

TEST_CASE("bare variables: extraction sized by the variable's type") {
    constexpr std::array<std::uint8_t, 7> b = {
        0xAA,
        0x34, 0x12,
        0x78, 0x56, 0x34, 0x12,
    };
    bytes::byte_reader br{b};
    std::uint8_t  a = 0;
    std::uint16_t c = 0;
    std::uint32_t d = 0;
    br >> a >> c >> d;
    CHECK(a == 0xAA);
    CHECK(c == 0x1234);
    CHECK(d == 0x12345678u);
    CHECK(br.position() == 7);
}

TEST_CASE("bare variables on big-endian reader") {
    constexpr std::array<std::uint8_t, 6> b = {0x12, 0x34, 0x12, 0x34, 0x56, 0x78};
    bytes::byte_reader_be br{b};
    std::uint16_t a = 0;
    std::uint32_t c = 0;
    br >> a >> c;
    CHECK(a == 0x1234);
    CHECK(c == 0x12345678u);
}

TEST_CASE("explicit endian tags override the reader default") {
    constexpr std::array<std::uint8_t, 4> b = {0x12, 0x34, 0x56, 0x78};
    bytes::byte_reader br_le{b};
    bytes::byte_reader_be br_be{b};

    std::uint16_t a = 0, c = 0;
    br_le >> bytes::u16be(a);   // LE reader, BE tag → BE
    br_be >> bytes::u16le(c);   // BE reader, LE tag → LE
    CHECK(a == 0x1234);
    CHECK(c == 0x3412);
}
