#pragma once

#include <onyx_anim/codecs/onyx_anim_codecs_export.h>
#include <onyx_anim/sdk/decoder.hh>

#include <memory>
#include <string_view>

namespace onyx_anim {

/**
 * RAD Game Tools Bink (Bink1) video decoder.
 *
 * Supports the modern revisions BIK[fghi] and BIK[k] — i.e. everything
 * except BIK[b] (the older variant with its own bundle layout) and BK2
 * (Bink 2, a different format with no open-source decoder anywhere).
 * Output is YUV 4:2:0 internally, converted to RGB888 at the surface
 * layer.
 */
class ONYX_ANIM_CODECS_EXPORT bink_decoder final {
public:
    static constexpr std::string_view codec_name = "bink";

    [[nodiscard]] static std::unique_ptr<anim_decoder> create();
};

} // namespace onyx_anim
