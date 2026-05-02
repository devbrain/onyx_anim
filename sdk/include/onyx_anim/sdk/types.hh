#pragma once

#include <onyx_anim/sdk/onyx_anim_sdk_export.h>
#include <onyx_anim/sdk/expected.hh>
#include <onyx_image/types.hpp>

#include <chrono>
#include <cstdint>

namespace onyx_anim {
    // Re-export the pixel_format from onyx_image so callers don't need both headers.
    using pixel_format = onyx_image::pixel_format;

    // ============================================================================
    // Animation metadata
    // ============================================================================

    struct anim_info {
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int frame_count = 0; // 0 if streamed/unknown
        pixel_format format = pixel_format::indexed8;
        std::chrono::microseconds duration{0}; // 0 if unknown
        std::chrono::microseconds frame_period{0}; // average; 0 if variable

        unsigned int audio_track_count = 0;
        unsigned int audio_rate = 0;
        unsigned int audio_channels = 0;
        unsigned int audio_bits = 0;
    };

    struct frame_info {
        unsigned int index = 0;
        std::chrono::microseconds pts{0};
        std::chrono::microseconds duration{0};
        bool palette_changed = false;
        bool keyframe = false;
        std::uint32_t user_flags = 0; // codec-specific cue bits
    };

    // ============================================================================
    // Decode options
    // ============================================================================

    struct decode_options {
        // Maximum allowed dimensions (0 = use default)
        unsigned int max_width = 16384;
        unsigned int max_height = 16384;

        // Preferred output pixel format. Codecs may ignore if they cannot honor it.
        // For SMK, default is rgb888; for FLC, indexed8 (palette-driven).
        pixel_format preferred_format = pixel_format::rgb888;

        // If true, eagerly build a frame index at open() time. Faster seeking,
        // slower open. Recommended for files that fit in memory.
        bool build_frame_index = true;
    };

    // Per-frame decode result alias.
    using frame_result = expected <frame_info, error_type>;
} // namespace onyx_anim
