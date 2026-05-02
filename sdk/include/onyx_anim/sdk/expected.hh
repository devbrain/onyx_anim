#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <version>

// Use std::expected when available (C++23), tl::expected otherwise.
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
    #include <expected>
    namespace onyx_anim {
        template<typename T, typename E>
        using expected = std::expected<T, E>;

        template<typename E>
        using unexpected = std::unexpected<E>;

        // Take by value so the helper works for lvalues, rvalues, AND callers
        // that pass an explicit template argument (e.g. make_unexpected<std::string>(lvalue)).
        // The pass-by-T forwarding-reference idiom rejects lvalues when the caller fixes E
        // explicitly, since `E&&` collapses to a plain rvalue reference there. One extra
        // move on the error path is the right tradeoff.
        template<typename E>
        constexpr auto make_unexpected(E e) {
            return std::unexpected<E>(std::move(e));
        }

        inline constexpr bool using_std_expected = true;
    }
#else
    #include <tl/expected.hpp>
    namespace onyx_anim {
        template<typename T, typename E>
        using expected = tl::expected<T, E>;

        template<typename E>
        using unexpected = tl::unexpected<E>;

        // Take by value so the helper works for lvalues, rvalues, AND callers
        // that pass an explicit template argument (e.g. make_unexpected<std::string>(lvalue)).
        // The pass-by-T forwarding-reference idiom rejects lvalues when the caller fixes E
        // explicitly, since `E&&` collapses to a plain rvalue reference there. One extra
        // move on the error path is the right tradeoff.
        template<typename E>
        constexpr auto make_unexpected(E e) {
            return tl::unexpected<E>(std::move(e));
        }

        inline constexpr bool using_std_expected = false;
    }
#endif

namespace onyx_anim {
    /// Error type used throughout onyx_anim — a human-readable description.
    using error_type = std::string;

    /// Common result type for void-returning operations.
    using result = expected<void, error_type>;
} // namespace onyx_anim
