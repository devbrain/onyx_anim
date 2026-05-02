#pragma once

#include <onyx_anim/codecs/onyx_anim_codecs_export.h>
#include <onyx_anim/sdk/decoder.hh>

#include <memory>
#include <string_view>

namespace onyx_anim {
    /**
     * Amiga IFF ANIM decoder — FORM ANIM { FORM ILBM ... } container.
     *
     * Currently supports:
     *   - keyframes (BMHD + CMAP + ByteRun1 BODY) at any bit depth 1..8 (EHB
     *     palette doubling honored; HAM modes deferred)
     *   - ANIM Op 5 delta frames (Byte Vertical Delta, by far the most common)
     *
     * Output format is indexed8.
     */
    class ONYX_ANIM_CODECS_EXPORT amiga_anim_decoder final {
        public:
            static constexpr std::string_view codec_name = "amiga_anim";
            [[nodiscard]] static std::unique_ptr<anim_decoder> create();
    };
} // namespace onyx_anim
