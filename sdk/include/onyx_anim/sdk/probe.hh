#pragma once

#include <onyx_anim/sdk/onyx_anim_sdk_export.h>
#include <onyx_anim/sdk/types.hh>

#include <musac/sdk/io_stream.hh>

#include <string>
#include <vector>

namespace onyx_anim {
    /**
     * Lightweight inspection result — everything an engine needs to decide
     * "should I play this clip, fall back to a thumbnail, or skip it?"
     * without actually committing to playback.
     */
    struct probe_info {
        std::string                   codec_name;
        anim_info                     video;
        std::vector<audio_track_info> audio_tracks;
    };

    /**
     * Sniff and parse just enough of `stream` to fill in a `probe_info`,
     * then leave the stream cursor where it found it so the caller can
     * still hand the same stream to `player::open` afterwards.
     *
     * Returns an error if no codec accepts the stream or header parsing
     * fails.
     */
    [[nodiscard]] ONYX_ANIM_SDK_EXPORT expected<probe_info, error_type>
        probe(musac::io_stream* stream);
} // namespace onyx_anim
