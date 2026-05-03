#pragma once

// Bink frame decoder (modern variant — `BIKf`, `BIKg`, `BIKh`, `BIKi`,
// `BIKk`). The older `BIKb` codepath (binkb_decode_plane) is intentionally
// not implemented yet — only one file in our corpus (DEFENDALL.BIK) uses
// it and it has its own bundle / quantization tables.
//
// The decoder maintains a persistent YUV(420) frame buffer pair (current
// + previous) so MOTION/RESIDUE/INTER blocks can fetch from the prior
// frame. Audio chunks are not consumed here — the caller (the bink.cc
// adapter) strips them off the front of each packet before invoking
// us with the video bit stream.

#include <bink/bundle.hh>
#include <bink/header.hh>
#include <bink/types.hh>

#include <cstdint>
#include <span>
#include <vector>

namespace bink {
    struct plane_buffers {
        // Three planes (Y, U, V) at the dimensions implied by the file
        // header. `stride` for each plane is the pixel width (no extra
        // padding) — we pass it explicitly to the IDCT/copy helpers.
        std::vector <std::uint8_t> y;
        std::vector <std::uint8_t> u;
        std::vector <std::uint8_t> v;
        unsigned int width  = 0;
        unsigned int height = 0;
    };

    struct frame_state {
        // Two planes — current frame's pixels are written into `cur`,
        // the previous frame stays in `prev`. We swap pointers per frame.
        plane_buffers a;
        plane_buffers b;
        plane_buffers* cur  = &a;
        plane_buffers* prev = &b;

        bundle_set bundles;
        unsigned int frame_num = 0;
    };

    // Allocate plane buffers + bundle storage for `width × height` frames.
    void frame_state_init(frame_state& fs,
                          unsigned int width, unsigned int height);

    // Decode one Bink video frame from the supplied bit stream into
    // fs.cur. After return, fs.cur holds the new frame and (cur ↔ prev)
    // are swapped for the next call. `version` is the BIK[?] revision.
    //
    // For BIK[b] (older variant) the caller should route through
    // `decode_frame_b` instead — it has a different bundle layout
    // (no per-frame Huffman trees, fixed bit-field widths) and a
    // different block-type vocabulary.
    result decode_frame(frame_state& fs,
                        std::span <const std::uint8_t> video_bits,
                        const file_header& h);

    // BIK[b] codepath. Same `frame_state` for plane buffers + bundle
    // storage, but the per-row dispatch is different and the
    // quantisation matrices are computed at first use.
    result decode_frame_b(frame_state& fs,
                          std::span <const std::uint8_t> video_bits,
                          const file_header& h);
} // namespace bink
