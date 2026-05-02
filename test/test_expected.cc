#include <doctest/doctest.h>

#include <onyx_anim/sdk/expected.hh>

#include <string>
#include <utility>

TEST_CASE("make_unexpected accepts an lvalue (deduced)") {
    std::string msg = "boom";
    auto r = onyx_anim::make_unexpected(msg);
    static_assert(std::is_same_v<decltype(r), onyx_anim::unexpected<std::string>>);
    CHECK(msg == "boom");                   // not stolen from
    CHECK(r.value() == "boom");
}

TEST_CASE("make_unexpected accepts an rvalue (deduced)") {
    auto r = onyx_anim::make_unexpected(std::string{"boom"});
    CHECK(r.value() == "boom");
}

TEST_CASE("make_unexpected accepts an lvalue with explicit template arg") {
    // Before the fix this failed with `cannot bind rvalue reference of type
    // std::string&& to lvalue of type std::string` because the forwarding-ref
    // signature collapsed to a plain rvalue ref under the explicit arg.
    std::string msg = "boom";
    auto r = onyx_anim::make_unexpected<std::string>(msg);
    CHECK(r.value() == "boom");
}

TEST_CASE("make_unexpected accepts an rvalue with explicit template arg") {
    auto r = onyx_anim::make_unexpected<std::string>(std::string{"boom"});
    CHECK(r.value() == "boom");
}

TEST_CASE("result default-fails round-trips") {
    auto fail = []() -> onyx_anim::result {
        return onyx_anim::make_unexpected<std::string>("fail");
    }();
    REQUIRE_FALSE(fail);
    CHECK(fail.error() == "fail");
}
