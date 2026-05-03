#pragma once

// Bridge lib_bink's local type names to onyx_anim's expected/error types.
// Free of musac/onyx_image deps. Bink is little-endian throughout — all
// container fields are LE u32s and the bit stream is LSB-first within each
// byte (matches ffmpeg's `#define BITSTREAM_READER_LE` mode used in
// libavcodec/bink.c).

#include <onyx_anim/sdk/expected.hh>
#include <bytes/bytes.hh>

#include <utility>

namespace bink {
    using error_type = onyx_anim::error_type;

    template <typename T>
    using expected = onyx_anim::expected<T, error_type>;

    using result = onyx_anim::result;

    template <typename E>
    constexpr auto make_unexpected(E e) {
        return onyx_anim::make_unexpected(std::move(e));
    }

    using byte_reader = bytes::byte_reader; // little-endian default
} // namespace bink
