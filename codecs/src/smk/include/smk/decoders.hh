#pragma once

// Smacker per-frame video and per-chunk audio decoders.
//
// The video decoder operates on a 4×4-block grid: for each block the TYPE
// tree emits a 16-bit code whose low 2 bits select MONO / FULL / SKIP /
// FILL block type, bits 2..7 select a run length from `block_runs[]`, and
// bits 8..15 (the "mode" byte) carry an extra payload — for FILL it's the
// solid colour, for FULL it's ignored.
//
// The audio decoder handles the two bit-pack flavours of compressed audio
// (8-bit and 16-bit, mono or stereo) plus raw uncompressed audio. Output is
// 8-bit unsigned bytes or 16-bit signed little-endian samples in the
// supplied output vector.

#include <smk/header.hh>
#include <smk/huffman.hh>
#include <smk/types.hh>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace smk {
    struct video_trees {
        big_tree mmap;
        big_tree mclr;
        big_tree full;
        big_tree type;
    };

    // Build the per-stream Huffman trees from the file's `trees_size`
    // bytes (located right after the 104-byte header). Sizes are taken
    // from the file_header so the bit-stream's "skipped tree" branches
    // can fall back to constant-zero trees of the right shape.
    expected<video_trees>
    build_video_trees(std::span <const std::uint8_t> trees_blob,
                      const file_header& h);

    // Decode a single video bit-stream (just the video portion — caller
    // strips the leading palette block and per-track audio chunks). The
    // output buffer must be width*height bytes; it's both read (for SKIP)
    // and written. Trees are mutated in place via the recency-cache shuffle
    // and at frame start via `last_reset` (handled internally).
    result decode_video_frame(video_trees& trees,
                              std::span <const std::uint8_t> frame_bits,
                              std::uint32_t width,
                              std::uint32_t height,
                              bool is_smk4,
                              std::uint8_t* out);

    // Update an in-place 768-byte palette using the leading bytes of a
    // frame's data. Returns the number of input bytes consumed (palette
    // block size including the 1-byte header). The first byte stores the
    // total chunk size in dwords; the rest is the 0x80 / 0x40 / new-entry
    // opcode stream documented in ffmpeg's smacker.c demuxer.
    expected<std::size_t>
    apply_palette_block(std::span <const std::uint8_t> data,
                        std::uint8_t* palette_768);

    // Audio decode. For raw (non-PACKED) tracks the payload is copied
    // verbatim into `out`; for PACKED tracks the bit stream is decoded
    // per ffmpeg's smka_decode_frame. Output format:
    //   8-bit  : uint8_t per sample
    //   16-bit : signed int16_t little-endian per sample
    // `out` is resized to `expected_unp_size` bytes (for PACKED) or to
    // the payload size (for raw).
    result decode_audio_chunk(const audio_track_info& track,
                              std::span <const std::uint8_t> chunk,
                              std::vector <std::uint8_t>& out);
} // namespace smk
