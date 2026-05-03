#pragma once

// Bridge lib_amiga_anim's local type names to onyx_anim's expected/error types.
// Free of musac/onyx_image deps; uses libiff (PUBLIC) for IFF parsing.

#include <onyx_anim/sdk/expected.hh>
#include <bytes/bytes.hh>

#include <type_traits>
#include <utility>

namespace anim {
    using error_type = onyx_anim::error_type;

    template <typename T>
    using expected = onyx_anim::expected<T, error_type>;

    using result = onyx_anim::result;

    template <typename E>
    constexpr auto make_unexpected(E&& e) {
        return onyx_anim::make_unexpected(std::forward<E>(e));
    }

    // ANIM is wholly big-endian (m68k Amiga heritage). Match the codec
    // pattern used by lib_atari_seq / lib_flc.
    using byte_reader = bytes::byte_reader_be;
    using bytes::u8;
    using bytes::u16;
    using bytes::u32;
    using bytes::s16;
    using bytes::s32;
    using bytes::skip;
} // namespace anim
