#pragma once

#include <onyx_anim/codecs/onyx_anim_codecs_export.h>
#include <onyx_anim/sdk/decoder.hh>

#include <memory>
#include <string_view>

namespace onyx_anim {
    /**
     * Autodesk Animator FLI/FLC decoder.
     *
     * Supports magic 0xAF11 (FLI, 320x200, 70Hz) and 0xAF12 (FLC, arbitrary size,
     * ms-based timing). Output format is always indexed8 with palette.
     */
    class ONYX_ANIM_CODECS_EXPORT flc_decoder final {
        public:
            static constexpr std::string_view codec_name = "flc";

            [[nodiscard]] static std::unique_ptr <anim_decoder> create();
    };
} // namespace onyx_anim
