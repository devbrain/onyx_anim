#pragma once

// Bridge lib_yafa's local type names to onyx_anim's expected/error types.
// Free of musac/onyx_image deps; YAFA is wholly big-endian.

#include <onyx_anim/sdk/expected.hh>
#include <bytes/bytes.hh>

#include <type_traits>
#include <utility>

namespace yafa {
    using error_type = onyx_anim::error_type;

    template <typename T>
    using expected = onyx_anim::expected<T, error_type>;

    using result = onyx_anim::result;

    template <typename E>
    constexpr auto make_unexpected(E&& e) {
        return onyx_anim::make_unexpected(std::forward<E>(e));
    }

    using byte_reader = bytes::byte_reader_be;
    using bytes::u8;
    using bytes::u16;
    using bytes::u32;
    using bytes::skip;
} // namespace yafa
