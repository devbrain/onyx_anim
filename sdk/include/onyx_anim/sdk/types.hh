#pragma once

#include <onyx_anim/sdk/onyx_anim_sdk_export.h>
#include <onyx_anim/sdk/expected.hh>
#include <onyx_image/types.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

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
        // Track 0's quick-access fields. For multi-track files, query
        // anim_decoder::audio_track(i) for full per-track info.
        unsigned int audio_rate = 0;
        unsigned int audio_channels = 0;
        unsigned int audio_bits = 0;
    };

    // ============================================================================
    // Per-track audio metadata
    // ============================================================================

    /**
     * Description of one of the file's audio tracks. Returned by
     * `anim_decoder::audio_track(index)` for inspection before deciding
     * which track to play (e.g. localized cutscenes with one track per
     * language, multi-stem files with music + SFX on separate tracks).
     */
    struct audio_track_info {
        unsigned int sample_rate = 0;            ///< Hz; 0 = absent
        unsigned int channels    = 0;            ///< 1 = mono, 2 = stereo
        unsigned int bits_per_sample = 0;        ///< usually 8 or 16
        std::chrono::microseconds duration{0};   ///< 0 if unknown / streamed
        // Free-form codec name, e.g. "Bink Audio (DCT)", "Bink Audio (RDFT)",
        // "Smacker DPCM", "Amiga 8SVX". Static string — no allocation, no
        // lifetime worries.
        const char* codec_name = "";
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

    // ============================================================================
    // Per-frame audio events (event-driven sound, e.g. ANIM+SLA SCTL triggers)
    // ============================================================================

    /**
     * One-shot audio trigger fired by the just-decoded video frame.
     *
     * The decoder hands the player a self-contained byte buffer that can be
     * wrapped with `musac::io_from_memory` and fed to a fresh musac decoder.
     * `sound_bytes` is shared because:
     *   - The same sample (e.g. an ANIM+SLA bank entry) may fire from many
     *     frames, and copying its body each time would be wasteful.
     *   - The player may keep the buffer alive past the next decode_frame()
     *     while a stream is still draining.
     */
    struct audio_event {
        // Raw bytes ready for `musac::io_from_memory` — typically a complete
        // IFF FORM (e.g. an 8SVX FORM with its leading "FORM"+size header).
        std::shared_ptr<const std::vector<std::uint8_t>> sound_bytes;

        float        volume        = 1.0f; ///< 0..1, linear
        std::uint16_t freq_override = 0u;  ///< 0 = use the format's native rate
        std::uint16_t repeats       = 1u;  ///< 0 = loop forever
        std::uint16_t channel_mask  = 0u;  ///< codec-specific; 0 = unspecified
    };
} // namespace onyx_anim
