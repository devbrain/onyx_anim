#pragma once

#include <yafa/types.hh>
#include <yafa/header.hh>

#include <cstdint>
#include <span>
#include <vector>

namespace yafa {
    // ------------------------------------------------------------------------
    // XPK chunk-walker. Validates the 36-byte XPK file header, then walks
    // the inner chunk list (8- or 12-byte chunk headers depending on the
    // file flags). Concatenates the per-chunk raw output and returns a
    // single buffer of `info.rawSize` bytes.
    //
    // `decompress_chunk` is the per-chunk worker, parameterised on
    // sub-library so we can plug in FAST / NUKE / etc.
    // ------------------------------------------------------------------------

    enum class xpk_sublib : std::uint32_t {
        none = 0,
        fast = 0x46415354u, // "FAST"
        nuke = 0x4E554B45u, // "NUKE"
    };

    [[nodiscard]] expected<std::vector<std::uint8_t>>
        xpk_decompress(std::span<const std::uint8_t> packed);

    // Sub-library decompressors operate on a single inner chunk's compressed
    // bytes and produce exactly `raw_size` bytes of output.
    [[nodiscard]] result xpk_fast_decompress(
        std::span<const std::uint8_t> packed,
        std::span<std::uint8_t>       raw);

    [[nodiscard]] result xpk_nuke_decompress(
        std::span<const std::uint8_t> packed,
        std::span<std::uint8_t>       raw);

    // ------------------------------------------------------------------------
    // YAFA delta — applies a delta-compressed frame onto a previous-frame
    // buffer. Per the spec the encoding is "similar to ANIM7": 8 plane
    // opcode-stream offsets, 8 plane data-stream offsets, then per-plane
    // column-walker (skip / unique / same opcodes). Column width is the
    // delta data-element width: byte=8, word=16, long=32 pixels.
    //
    // `prev` is the destination/source buffer (already holding frame N-2
    // for double-buffered playback); the delta is applied in place.
    // The buffer layout is YAFA-planar: plane 0 occupies the first
    // `bytes_per_plane` bytes contiguously, then plane 1, etc.
    // ------------------------------------------------------------------------
    [[nodiscard]] result yafa_apply_delta(
        std::span<const std::uint8_t> delta,
        std::uint8_t*                 prev,
        unsigned int                  width,
        unsigned int                  height,
        unsigned int                  planes,
        delta_kind                    width_kind);
} // namespace yafa
