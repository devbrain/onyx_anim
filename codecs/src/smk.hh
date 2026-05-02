#pragma once

#include <onyx_anim/codecs/onyx_anim_codecs_export.h>
#include <onyx_anim/sdk/decoder.hh>

#include <memory>
#include <string_view>

namespace onyx_anim {

/**
 * RAD Game Tools Smacker decoder.
 *
 * Supports SMK2 and SMK4. Up to 7 audio tracks, output format defaults to
 * rgb888 (set decode_options::preferred_format to indexed8 to receive raw
 * Y-plane indices instead — useful for palette-cycling effects).
 */
class ONYX_ANIM_CODECS_EXPORT smk_decoder final {
public:
    static constexpr std::string_view codec_name = "smk";

    [[nodiscard]] static std::unique_ptr<anim_decoder> create();
};

} // namespace onyx_anim

