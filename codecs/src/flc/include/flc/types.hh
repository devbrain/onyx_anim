#pragma once

// Bridge lib_flc's local type names to the onyx_anim SDK's expected/error types
// and lib_bytes's byte_reader, so all flc headers and impls can write
// `expected<T>`, `result`, `make_unexpected(...)`, `byte_reader`, `u16le(v)`,
// etc. without spelling out the originating namespaces every time.

#include <onyx_anim/sdk/expected.hh>
#include <bytes/bytes.hh>

#include <type_traits>
#include <utility>

namespace flc {
    using error_type = onyx_anim::error_type;

    template <typename T>
    using expected = onyx_anim::expected<T, error_type>;

    using result = onyx_anim::result;

    template <typename E>
    constexpr auto make_unexpected(E&& e) {
        return onyx_anim::make_unexpected(std::forward<E>(e));
    }

    // Byte-reading conveniences — see <bytes/bytes.hh>.
    // FLC is wholly little-endian, so the default-endian byte_reader (LE)
    // covers everything via the short u8/u16/u32 tags.
    using bytes::byte_reader;
    using bytes::u8;
    using bytes::u16;
    using bytes::u32;
    using bytes::s16;
    using bytes::s32;
    using bytes::u16le;
    using bytes::u32le;
    using bytes::u16be;
    using bytes::u32be;
    using bytes::s16le;
    using bytes::s32le;
    using bytes::s16be;
    using bytes::s32be;
    using bytes::skip;
} // namespace flc
