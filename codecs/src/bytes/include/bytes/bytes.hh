#pragma once

// Span-based binary I/O helpers for codec parsers.
//
// Two layers:
//   1. Unchecked pointer readers — read_u16le(p), etc. Inline, fast, suitable
//      for inner loops where bounds were validated once at the chunk boundary.
//   2. basic_byte_reader<DefaultEndian> — wraps a std::span<const uint8_t>
//      with a cursor and a sticky failure flag, supporting operator>> with
//      tag factories.
//
//      `byte_reader`    — alias for basic_byte_reader<std::endian::little>
//      `byte_reader_be` — alias for basic_byte_reader<std::endian::big>
//
//      Tag families:
//        u8(v)                  endian-agnostic
//        u16(v),  u32(v),  ...  reads using the reader's default endianness
//        u16le(v), u16be(v), ... explicit override regardless of default
//        skip(n)                advance cursor
//
// Mixed-endianness formats can use one reader with the dominant default plus
// explicit *le / *be tags at the few odd fields. Single-endianness formats
// just pick the right alias and use the short tags throughout.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bytes {

    // ========================================================================
    // Unchecked pointer readers
    // ========================================================================
    //
    // Preconditions: the pointer must address at least sizeof(result) bytes.
    // No bounds checking; this is the fast path for inner loops.

    [[nodiscard]] constexpr std::uint8_t read_u8(const std::uint8_t* p) noexcept {
        return *p;
    }

    [[nodiscard]] constexpr std::uint16_t read_u16le(const std::uint8_t* p) noexcept {
        return static_cast<std::uint16_t>(
            static_cast<unsigned>(p[0]) |
            (static_cast<unsigned>(p[1]) << 8u));
    }

    [[nodiscard]] constexpr std::uint16_t read_u16be(const std::uint8_t* p) noexcept {
        return static_cast<std::uint16_t>(
            (static_cast<unsigned>(p[0]) << 8u) |
            static_cast<unsigned>(p[1]));
    }

    [[nodiscard]] constexpr std::uint32_t read_u32le(const std::uint8_t* p) noexcept {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8u) |
               (static_cast<std::uint32_t>(p[2]) << 16u) |
               (static_cast<std::uint32_t>(p[3]) << 24u);
    }

    [[nodiscard]] constexpr std::uint32_t read_u32be(const std::uint8_t* p) noexcept {
        return (static_cast<std::uint32_t>(p[0]) << 24u) |
               (static_cast<std::uint32_t>(p[1]) << 16u) |
               (static_cast<std::uint32_t>(p[2]) << 8u) |
               static_cast<std::uint32_t>(p[3]);
    }

    [[nodiscard]] constexpr std::int16_t read_s16le(const std::uint8_t* p) noexcept {
        return static_cast<std::int16_t>(read_u16le(p));
    }
    [[nodiscard]] constexpr std::int16_t read_s16be(const std::uint8_t* p) noexcept {
        return static_cast<std::int16_t>(read_u16be(p));
    }
    [[nodiscard]] constexpr std::int32_t read_s32le(const std::uint8_t* p) noexcept {
        return static_cast<std::int32_t>(read_u32le(p));
    }
    [[nodiscard]] constexpr std::int32_t read_s32be(const std::uint8_t* p) noexcept {
        return static_cast<std::int32_t>(read_u32be(p));
    }

    // ========================================================================
    // Tag factories
    // ========================================================================

    namespace detail {
        // Endian-agnostic
        struct read_u8_t  { std::uint8_t& v; };
        struct skip_t     { std::size_t   n; };

        // Default-endian — operator>> dispatches based on the reader's template arg
        struct read_u16_t { std::uint16_t& v; };
        struct read_u32_t { std::uint32_t& v; };
        struct read_s16_t { std::int16_t&  v; };
        struct read_s32_t { std::int32_t&  v; };

        // Explicit override
        struct read_u16le_t{ std::uint16_t& v; };
        struct read_u32le_t{ std::uint32_t& v; };
        struct read_u16be_t{ std::uint16_t& v; };
        struct read_u32be_t{ std::uint32_t& v; };
        struct read_s16le_t{ std::int16_t&  v; };
        struct read_s32le_t{ std::int32_t&  v; };
        struct read_s16be_t{ std::int16_t&  v; };
        struct read_s32be_t{ std::int32_t&  v; };
    }

    [[nodiscard]] inline detail::read_u8_t  u8(std::uint8_t& v)   noexcept { return {v}; }
    [[nodiscard]] inline detail::skip_t     skip(std::size_t n)   noexcept { return {n}; }

    [[nodiscard]] inline detail::read_u16_t u16(std::uint16_t& v) noexcept { return {v}; }
    [[nodiscard]] inline detail::read_u32_t u32(std::uint32_t& v) noexcept { return {v}; }
    [[nodiscard]] inline detail::read_s16_t s16(std::int16_t&  v) noexcept { return {v}; }
    [[nodiscard]] inline detail::read_s32_t s32(std::int32_t&  v) noexcept { return {v}; }

    [[nodiscard]] inline detail::read_u16le_t u16le(std::uint16_t& v) noexcept { return {v}; }
    [[nodiscard]] inline detail::read_u32le_t u32le(std::uint32_t& v) noexcept { return {v}; }
    [[nodiscard]] inline detail::read_u16be_t u16be(std::uint16_t& v) noexcept { return {v}; }
    [[nodiscard]] inline detail::read_u32be_t u32be(std::uint32_t& v) noexcept { return {v}; }
    [[nodiscard]] inline detail::read_s16le_t s16le(std::int16_t&  v) noexcept { return {v}; }
    [[nodiscard]] inline detail::read_s32le_t s32le(std::int32_t&  v) noexcept { return {v}; }
    [[nodiscard]] inline detail::read_s16be_t s16be(std::int16_t&  v) noexcept { return {v}; }
    [[nodiscard]] inline detail::read_s32be_t s32be(std::int32_t&  v) noexcept { return {v}; }

    // ========================================================================
    // basic_byte_reader: cursor over a span with sticky failure
    // ========================================================================

    template <std::endian DefaultEndian>
    class basic_byte_reader {
    public:
        static constexpr std::endian default_endian = DefaultEndian;

        constexpr explicit basic_byte_reader(std::span<const std::uint8_t> data) noexcept
            : data_(data) {}

        [[nodiscard]] constexpr std::size_t position() const noexcept { return pos_; }
        [[nodiscard]] constexpr std::size_t size() const noexcept     { return data_.size(); }
        [[nodiscard]] constexpr std::size_t remaining() const noexcept {
            return data_.size() - pos_;
        }
        [[nodiscard]] constexpr bool has(std::size_t n) const noexcept {
            return !failed_ && remaining() >= n;
        }
        [[nodiscard]] constexpr bool failed() const noexcept { return failed_; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return !failed_;
        }

        [[nodiscard]] constexpr const std::uint8_t* peek() const noexcept {
            return data_.data() + pos_;
        }
        [[nodiscard]] constexpr std::span<const std::uint8_t> remaining_span() const noexcept {
            return data_.subspan(pos_);
        }

        constexpr void mark_failed() noexcept { failed_ = true; }

        [[nodiscard]] std::span<const std::uint8_t> take(std::size_t n) noexcept {
            if (!consume(n)) return {};
            return data_.subspan(pos_ - n, n);
        }

        // Explicit-endian direct readers --------------------------------------

        [[nodiscard]] std::uint8_t  read_u8()    noexcept { return consume(1) ? bytes::read_u8   (data_.data() + pos_ - 1) : 0; }
        [[nodiscard]] std::uint16_t read_u16le() noexcept { return consume(2) ? bytes::read_u16le(data_.data() + pos_ - 2) : 0; }
        [[nodiscard]] std::uint16_t read_u16be() noexcept { return consume(2) ? bytes::read_u16be(data_.data() + pos_ - 2) : 0; }
        [[nodiscard]] std::uint32_t read_u32le() noexcept { return consume(4) ? bytes::read_u32le(data_.data() + pos_ - 4) : 0; }
        [[nodiscard]] std::uint32_t read_u32be() noexcept { return consume(4) ? bytes::read_u32be(data_.data() + pos_ - 4) : 0; }
        [[nodiscard]] std::int16_t  read_s16le() noexcept { return static_cast<std::int16_t>(read_u16le()); }
        [[nodiscard]] std::int16_t  read_s16be() noexcept { return static_cast<std::int16_t>(read_u16be()); }
        [[nodiscard]] std::int32_t  read_s32le() noexcept { return static_cast<std::int32_t>(read_u32le()); }
        [[nodiscard]] std::int32_t  read_s32be() noexcept { return static_cast<std::int32_t>(read_u32be()); }

        // Default-endian direct readers (compile-time dispatched) -------------

        [[nodiscard]] std::uint16_t read_u16() noexcept {
            if constexpr (DefaultEndian == std::endian::little) return read_u16le();
            else                                                 return read_u16be();
        }
        [[nodiscard]] std::uint32_t read_u32() noexcept {
            if constexpr (DefaultEndian == std::endian::little) return read_u32le();
            else                                                 return read_u32be();
        }
        [[nodiscard]] std::int16_t  read_s16() noexcept { return static_cast<std::int16_t>(read_u16()); }
        [[nodiscard]] std::int32_t  read_s32() noexcept { return static_cast<std::int32_t>(read_u32()); }

        // operator>> direct overloads (size and endianness inferred from the
        // variable's type, using the reader's default endianness for u16/u32):
        //
        //     br >> h.foo32 >> h.foo16 >> h.fooU8;
        //
        // Tag wrappers below remain available for the explicit *le / *be cases
        // and for skip(n), which has no natural lvalue.

        basic_byte_reader& operator>>(std::uint8_t&  v) noexcept { v = read_u8();  return *this; }
        basic_byte_reader& operator>>(std::int8_t&   v) noexcept { v = static_cast<std::int8_t>(read_u8()); return *this; }
        basic_byte_reader& operator>>(std::uint16_t& v) noexcept { v = read_u16(); return *this; }
        basic_byte_reader& operator>>(std::int16_t&  v) noexcept { v = read_s16(); return *this; }
        basic_byte_reader& operator>>(std::uint32_t& v) noexcept { v = read_u32(); return *this; }
        basic_byte_reader& operator>>(std::int32_t&  v) noexcept { v = read_s32(); return *this; }

        // operator>> tag overloads --------------------------------------------

        // Endian-agnostic
        basic_byte_reader& operator>>(detail::read_u8_t t) noexcept { t.v = read_u8();    return *this; }
        basic_byte_reader& operator>>(detail::skip_t    t) noexcept { (void)consume(t.n); return *this; }

        // Default-endian
        basic_byte_reader& operator>>(detail::read_u16_t t) noexcept { t.v = read_u16(); return *this; }
        basic_byte_reader& operator>>(detail::read_u32_t t) noexcept { t.v = read_u32(); return *this; }
        basic_byte_reader& operator>>(detail::read_s16_t t) noexcept { t.v = read_s16(); return *this; }
        basic_byte_reader& operator>>(detail::read_s32_t t) noexcept { t.v = read_s32(); return *this; }

        // Explicit override
        basic_byte_reader& operator>>(detail::read_u16le_t t) noexcept { t.v = read_u16le(); return *this; }
        basic_byte_reader& operator>>(detail::read_u32le_t t) noexcept { t.v = read_u32le(); return *this; }
        basic_byte_reader& operator>>(detail::read_u16be_t t) noexcept { t.v = read_u16be(); return *this; }
        basic_byte_reader& operator>>(detail::read_u32be_t t) noexcept { t.v = read_u32be(); return *this; }
        basic_byte_reader& operator>>(detail::read_s16le_t t) noexcept { t.v = read_s16le(); return *this; }
        basic_byte_reader& operator>>(detail::read_s32le_t t) noexcept { t.v = read_s32le(); return *this; }
        basic_byte_reader& operator>>(detail::read_s16be_t t) noexcept { t.v = read_s16be(); return *this; }
        basic_byte_reader& operator>>(detail::read_s32be_t t) noexcept { t.v = read_s32be(); return *this; }

    private:
        bool consume(std::size_t n) noexcept {
            if (failed_ || remaining() < n) {
                failed_ = true;
                return false;
            }
            pos_ += n;
            return true;
        }

        std::span<const std::uint8_t> data_;
        std::size_t pos_ = 0;
        bool failed_ = false;
    };

    using byte_reader    = basic_byte_reader<std::endian::little>;
    using byte_reader_be = basic_byte_reader<std::endian::big>;

} // namespace bytes
