#pragma once

#include <dpan/types.hh>

#include <cstdint>
#include <span>

namespace dpan {
    // Decompress one record's RunSkipDump-encoded data into an indexed8
    // chunky framebuffer. Mirrors ffmpeg's libavcodec/anm.c::decode_frame
    // exactly, including the subtle x-tracking that lets COPY operations
    // span row boundaries.
    //
    // The input is the full record payload (not just the post-header part —
    // we eat the leading 0x42 IDnum and 4-byte preamble ourselves).
    //
    // `dst` must be at least `width * height` bytes; the row stride is
    // `width` (chunky 8-bit, no padding). Records are *applied* on top of
    // the previous frame: pixels not touched by COPY/FILL ops keep the
    // previous frame's value (this is RunSkipDump's whole point — most
    // records skip large areas).
    [[nodiscard]] result decompress_record(
        std::span<const std::uint8_t> record,
        std::uint8_t*                 dst,
        unsigned int                  width,
        unsigned int                  height);
} // namespace dpan
