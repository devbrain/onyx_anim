#pragma once

#include <onyx_anim/codecs/onyx_anim_codecs_export.h>
#include <onyx_anim/sdk/decoder.hh>

#include <memory>
#include <string_view>

namespace onyx_anim {
    /**
     * YAFA animation decoder.
     *
     * "Yet Another File-format for Animation" — IFF-based raster format from
     * 1996 (Andreas Maschke / Michael Henke). Frames may be planar or chunky
     * 8-bit, optionally XPK-compressed (FAST and NUKE sub-libraries
     * supported), optionally HAM6/HAM8, with an optional per-frame palette
     * ("dynamic palette") and optional byte/word/long delta compression.
     */
    class ONYX_ANIM_CODECS_EXPORT yafa_decoder final {
        public:
            static constexpr std::string_view codec_name = "yafa";

            [[nodiscard]] static std::unique_ptr<anim_decoder> create();
    };
} // namespace onyx_anim
