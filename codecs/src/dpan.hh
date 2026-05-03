#pragma once

#include <onyx_anim/codecs/onyx_anim_codecs_export.h>
#include <onyx_anim/sdk/decoder.hh>

#include <memory>
#include <string_view>

namespace onyx_anim {
    /**
     * Deluxe Paint Animation (.ANM) decoder.
     *
     * DOS-era 256-colour raster format using the LPF "Large Page" container
     * — frames live inside 64-KiB pages and use RunSkipDump (compression
     * type 1) for inter-frame deltas. Always 8 bpp with a 256-entry RGB
     * palette stored in the file header.
     */
    class ONYX_ANIM_CODECS_EXPORT dpan_decoder final {
        public:
            static constexpr std::string_view codec_name = "dpan";

            [[nodiscard]] static std::unique_ptr<anim_decoder> create();
    };
} // namespace onyx_anim
