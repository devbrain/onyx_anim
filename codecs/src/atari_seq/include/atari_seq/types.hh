#pragma once

// Bridge lib_atari_seq's local type names to the onyx_anim SDK's expected/error
// types and lib_bytes' byte_reader (big-endian default — Atari ST is m68k).

#include <onyx_anim/sdk/expected.hh>
#include <bytes/bytes.hh>

#include <type_traits>
#include <utility>

namespace atari_seq {
    using error_type = onyx_anim::error_type;

    template <typename T>
    using expected = onyx_anim::expected<T, error_type>;

    using result = onyx_anim::result;

    template <typename E>
    constexpr auto make_unexpected(E&& e) {
        return onyx_anim::make_unexpected(std::forward<E>(e));
    }

    // SEQ is wholly big-endian. The default-endian byte_reader_be picks BE
    // for u16/u32 reads; explicit *be / *le tags remain available if needed.
    using byte_reader = bytes::byte_reader_be;
    using bytes::u8;
    using bytes::u16;
    using bytes::u32;
    using bytes::s16;
    using bytes::s32;
    using bytes::u16le;
    using bytes::u32le;
    using bytes::u16be;
    using bytes::u32be;
    using bytes::skip;
} // namespace atari_seq
