#pragma once

// Bink Audio (RDFT + DCT variants) decoder.
//
// Ported from ffmpeg's libavcodec/binkaudio.c. Both per-band-quantized
// transforms produce float samples; this module decodes a packet into
// floats and a separate adapter glue converts to whatever PCM format
// the pcm_audio_decoder shell expects.
//
// The transform itself is delegated to PocketFFT (lib_pocketfft):
//   - RDFT path: complex-to-real inverse DFT, length = frame_len
//   - DCT path:  inverse DCT-III ("DCT" with inverse direction in
//                ffmpeg's libavutil/tx terms), length = frame_len / 2
//
// Bink Audio is internally interleaved across MAX_CHANNELS=2 channel
// pairs: a clip with > 2 channels stores them as multiple stacked
// pair-streams. We model that by tracking a `ch_offset` between
// `decode_packet` calls; the caller is expected to feed packets in
// order and accumulate float output across calls.

#include <bink/bit_reader.hh>
#include <bink/types.hh>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bink {
    inline constexpr int kAudioMaxDctChannels = 6;
    inline constexpr int kAudioMaxChannels = 2; // MAX_CHANNELS

    struct audio_decoder {
        // Per-stream invariants set at init().
        int sample_rate = 0;
        int channels    = 0;     // total channels (1..6)
        int frame_len   = 0;     // transform size in samples
        int overlap_len = 0;     // overlap region (frame_len/16)
        int block_size  = 0;     // samples per packet block
        int num_bands   = 0;
        bool use_dct    = false; // false = RDFT
        bool version_b  = false; // BIK[b] uses raw float words instead of get_float
        float root      = 0.0f;
        std::array <float,    96> quant_table{};
        std::array <unsigned, 26> bands{};

        // Per-channel "previous frame's tail" for overlap-add.
        std::array <std::array <float, 256>, kAudioMaxDctChannels> previous{};
        bool first = true;

        // Per-call state — `ch_offset` advances by kAudioMaxChannels each
        // decode_block call, wrapping at `channels`. Reset between
        // packets via reset_for_new_packet().
        int  ch_offset = 0;
    };

    // Set up the per-stream invariants. `sample_rate`, `channels`, and
    // `use_dct` come from the audio_track header parsed by header.cc.
    // `version_b` is tied to the file-level Bink revision.
    [[nodiscard]] result audio_init(audio_decoder& s,
                                    int sample_rate, int channels,
                                    bool use_dct, bool version_b);

    // Decode one Bink Audio packet and append `block_size`-worth of
    // interleaved-as-needed float samples to `out` for each block in
    // the packet. The packet starts with a 4-byte "reported size" prefix
    // we skip over.
    [[nodiscard]] result audio_decode_packet(audio_decoder& s,
                                             std::span <const std::uint8_t> packet,
                                             std::vector <float>& out);
} // namespace bink
