#pragma once

#include <onyx_anim/codecs/onyx_anim_codecs_export.h>
#include <onyx_anim/sdk/decoder.hh>

#include <memory>
#include <string_view>

namespace onyx_anim {
    /**
     * Atari ST Cyber Paint .SEQ animation decoder.
     *
     * Supports magic 0xFEDB (Cyber Paint) and 0xFEDC (Flicker), 320×200
     * 16-color resolution. Output format is indexed8 with palette.
     */
    class flc_decoder; // fwd-decl avoidance not needed; stub here for symmetry

    class ONYX_ANIM_CODECS_EXPORT atari_seq_decoder final {
        public:
            static constexpr std::string_view codec_name = "atari_seq";

            [[nodiscard]] static std::unique_ptr<anim_decoder> create();
    };
} // namespace onyx_anim
