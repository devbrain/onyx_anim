#pragma once

#include <failsafe/expected.hh>

namespace onyx_anim {
    template<typename T, typename E = failsafe::error_type>
    using expected = failsafe::expected<T, E>;

    template<typename E>
    using unexpected = failsafe::unexpected<E>;

    using failsafe::unexpect_t;
    using failsafe::unexpect;
    using failsafe::bad_expected_access;

    using failsafe::make_unexpected;
    using failsafe::make_unexpectedf;

    using failsafe::using_std_expected;
    using failsafe::expected_implementation;

    /// Error type used throughout onyx_anim — a human-readable description.
    using error_type = failsafe::error_type;

    /// Common result type for void-returning operations.
    using result = failsafe::result;
} // namespace onyx_anim
