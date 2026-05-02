#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/codecs/register_codecs.hh>

namespace {
    struct codec_registration {
        codec_registration() {
            onyx_anim::register_all_codecs(onyx_anim::codec_registry::instance());
        }
    };
    const codec_registration g_register{};
}
