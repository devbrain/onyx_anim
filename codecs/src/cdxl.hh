#pragma once

#include <onyx_anim/codecs/onyx_anim_codecs_export.h>
#include <onyx_anim/sdk/decoder.hh>

#include <memory>
#include <string_view>

namespace onyx_anim {
    /**
     * Commodore CDXL animation decoder.
     *
     * Plays the CDXL container familiar from the Amiga CDTV: a flat sequence
     * of self-contained frame chunks, each carrying a 32-byte header, an
     * optional 12-bit-RGB colormap, raw bit-planar pixel data, and (optionally)
     * a slice of mono/stereo signed 8-bit PCM audio.
     *
     * Supported video modes: RGB indexed (1..8 planes, BIT_PLANAR) → indexed8;
     * HAM6 / HAM8 → rgb888. Chunky and BIT/BYTE_LINE layouts are not handled.
     */
    class ONYX_ANIM_CODECS_EXPORT cdxl_decoder final {
        public:
            static constexpr std::string_view codec_name = "cdxl";

            [[nodiscard]] static std::unique_ptr<anim_decoder> create();
    };
} // namespace onyx_anim
