#pragma once

// Bridge lib_dpan's local type names to onyx_anim's expected/error types.
// Free of musac/onyx_image deps. DPaintAnim is little-endian throughout
// (DOS/x86 origin) — that's the whole reason it has its own namespace
// instead of folding into one of the BE codecs.

#include <onyx_anim/sdk/expected.hh>
#include <bytes/bytes.hh>

#include <type_traits>
#include <utility>

namespace dpan {
    using error_type = onyx_anim::error_type;

    template <typename T>
    using expected = onyx_anim::expected<T, error_type>;

    using result = onyx_anim::result;

    template <typename E>
    constexpr auto make_unexpected(E&& e) {
        return onyx_anim::make_unexpected(std::forward<E>(e));
    }

    using byte_reader = bytes::byte_reader;          // little-endian default
    using bytes::u8;
    using bytes::u16;
    using bytes::u32;
    using bytes::skip;
} // namespace dpan
