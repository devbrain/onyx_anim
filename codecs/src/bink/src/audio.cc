#include <bink/audio.hh>

#include <pocketfft_hdronly.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace bink {
    namespace {
        // ff_wma_critical_freqs — psychoacoustic critical-band edges in
        // Hz. Used to derive per-stream `bands[]` count and edges.
        constexpr std::uint16_t kCriticalFreqs[25] = {
              100,   200,  300,  400,  510,  630,   770,   920,
             1080,  1270, 1480, 1720, 2000, 2320,  2700,  3150,
             3700,  4400, 5300, 6400, 7700, 9500, 12000, 15500,
            24500,
        };

        constexpr std::uint8_t kRleLengthTab[16] = {
            2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64
        };

        // Read a Bink Audio float word from the bit stream: 5-bit power,
        // 23-bit mantissa, 1-bit sign — produced by ffmpeg's
        // `get_float()`.
        float read_audio_float(bit_reader& br) {
            const int power = static_cast <int>(br.get_bits(5));
            const std::uint32_t mant = br.get_bits(23);
            float f = std::ldexp(static_cast <float>(mant), power - 23);
            if (br.get_bit() != 0u) f = -f;
            return f;
        }

        unsigned int log2u(unsigned int v) noexcept {
            unsigned int r = 0;
            while (v > 1) { v >>= 1u; ++r; }
            return r;
        }

        // ---- pocketfft glue ----
        //
        // We allocate one shape={size_t} pocketfft `shape_t` per call;
        // pocketfft is lightweight enough that recreating the descriptor
        // per packet is fine for our throughput needs.

        // Inverse RDFT: input is a packed real DFT (N/2+1 complex bins,
        // packed as 2*(N/2+1) floats — re/im pairs), output is N real
        // floats. `coeffs` length must be N+2.
        void inverse_rdft(float* coeffs, std::size_t N) {
            using namespace pocketfft;
            const shape_t shape{N};
            const stride_t stride_real{static_cast<std::ptrdiff_t>(sizeof(float))};
            const stride_t stride_complex{
                static_cast<std::ptrdiff_t>(sizeof(std::complex<float>))};
            const shape_t axes{0};
            // Treat coeffs as `N/2+1` complex; output back into the same
            // buffer interpreted as N real floats. PocketFFT's c2r runs
            // an inverse DFT yielding N real samples scaled by N (not
            // 1/N) — we apply a 0.5 pre-scale below to match ffmpeg's
            // tx_init scale.
            const auto* in = reinterpret_cast<const std::complex<float>*>(coeffs);
            c2r(shape, stride_complex, stride_real, axes, /*forward=*/false,
                in, coeffs, /*fct=*/0.5f);
        }

        // Inverse DCT-III with normalisation. PocketFFT's `dct` accepts
        // a `type` parameter (1..4); type 3 is what ffmpeg's
        // AV_TX_FLOAT_DCT-with-inverse=1 produces. Scale = 1.
        void inverse_dct(float* coeffs, std::size_t N) {
            using namespace pocketfft;
            const shape_t shape{N};
            const stride_t stride_real{static_cast<std::ptrdiff_t>(sizeof(float))};
            const shape_t axes{0};
            // pocketfft's `dct` with type=3 + ortho=false gives an
            // inverse DCT-II (which equals DCT-III/N up to scale).
            // ffmpeg applies scale = 1/(N) externally; we fold the same
            // factor into pocketfft's `fct`.
            const float fct = 1.0f / static_cast <float>(N);
            dct(shape, stride_real, stride_real, axes,
                /*type=*/3, coeffs, coeffs, fct, /*ortho=*/false);
        }
    } // namespace

    result audio_init(audio_decoder& s,
                      int sample_rate, int channels,
                      bool use_dct, bool version_b) {
        if (channels < 1 ||
            channels > (use_dct ? kAudioMaxDctChannels : kAudioMaxChannels)) {
            return make_unexpected<error_type>("bink: invalid audio channel count");
        }

        // ffmpeg picks frame_len_bits from the sample rate. RDFT path
        // additionally adjusts for channel count when version != 'b'.
        int frame_len_bits;
        if (sample_rate < 22050)      frame_len_bits =  9;
        else if (sample_rate < 44100) frame_len_bits = 10;
        else                          frame_len_bits = 11;

        int effective_rate = sample_rate;
        if (!use_dct) {
            // RDFT: audio is interleaved, processed as 1 channel at the
            // higher rate.
            if (effective_rate > (1 << 30) / channels) {
                return make_unexpected<error_type>("bink: audio rate * channels overflow");
            }
            effective_rate *= channels;
            if (!version_b) {
                frame_len_bits +=
                    static_cast <int>(log2u(static_cast <unsigned int>(channels)));
            }
            // Internal channel count for the decoder's own per-channel
            // state (only ever 1 for RDFT).
            s.channels = 1;
        } else {
            s.channels = channels;
        }

        s.sample_rate = sample_rate;
        s.use_dct     = use_dct;
        s.version_b   = version_b;
        s.frame_len   = 1 << frame_len_bits;
        s.overlap_len = s.frame_len / 16;
        const int per_call_channels = std::min(kAudioMaxChannels, s.channels);
        s.block_size  = (s.frame_len - s.overlap_len) * per_call_channels;
        const int sample_rate_half = (effective_rate + 1) / 2;

        s.root = use_dct
            ? static_cast <float>(s.frame_len) /
                  (std::sqrt(static_cast <float>(s.frame_len)) * 32768.0f)
            : 2.0f / (std::sqrt(static_cast <float>(s.frame_len)) * 32768.0f);

        for (int i = 0; i < 96; ++i) {
            // 0.066399999/log10(M_E) = 0.15289164787...
            s.quant_table[i] = std::exp(i * 0.15289164787221953823f) * s.root;
        }

        s.num_bands = 1;
        while (s.num_bands < 25 &&
               sample_rate_half > kCriticalFreqs[s.num_bands - 1]) {
            ++s.num_bands;
        }

        s.bands[0] = 2u;
        for (int i = 1; i < s.num_bands; ++i) {
            s.bands[i] = (static_cast <unsigned int>(kCriticalFreqs[i - 1]) *
                          static_cast <unsigned int>(s.frame_len) /
                          static_cast <unsigned int>(sample_rate_half)) & ~1u;
        }
        s.bands[s.num_bands] = static_cast <unsigned int>(s.frame_len);

        s.first = true;
        s.ch_offset = 0;
        for (auto& row : s.previous) row.fill(0.0f);

        return {};
    }

    namespace {
        // Decode one block (one channel-pair) and append output samples.
        // ffmpeg's `decode_block` writes per-channel float arrays;
        // because Bink Audio packs MAX_CHANNELS=2 at a time, we write
        // ch_offset .. ch_offset+local_channels into per-channel
        // intermediate buffers and the caller interleaves them at the
        // end of the packet.
        result decode_block(audio_decoder& s, bit_reader& br,
                            int local_channels,
                            std::vector <std::vector <float>>& per_channel_acc) {
            // Working buffer for the transform — frame_len real samples
            // for the DCT path, frame_len + 2 floats for the RDFT path
            // (real-DFT layout has N/2+1 complex bins = N+2 floats).
            std::vector <float> coeffs(static_cast <std::size_t>(s.frame_len + 2), 0.0f);

            if (s.use_dct) {
                if (br.bits_left() < 2u) {
                    return make_unexpected<error_type>("bink: DCT skip-bits truncated");
                }
                br.skip_bits(2);
            }

            for (int ch = 0; ch < local_channels; ++ch) {
                if (s.version_b) {
                    if (br.bits_left() < 64u) {
                        return make_unexpected<error_type>("bink: BIK[b] coeffs truncated");
                    }
                    const std::uint32_t w0 = br.get_bits(32);
                    const std::uint32_t w1 = br.get_bits(32);
                    float f0, f1;
                    std::memcpy(&f0, &w0, sizeof(f0));
                    std::memcpy(&f1, &w1, sizeof(f1));
                    coeffs[0] = f0 * s.root;
                    coeffs[1] = f1 * s.root;
                } else {
                    if (br.bits_left() < 58u) {
                        return make_unexpected<error_type>("bink: float coeffs truncated");
                    }
                    coeffs[0] = read_audio_float(br) * s.root;
                    coeffs[1] = read_audio_float(br) * s.root;
                }

                if (br.bits_left() <
                    static_cast <std::size_t>(s.num_bands) * 8u) {
                    return make_unexpected<error_type>("bink: band quant table truncated");
                }
                std::array <float, 25> quant{};
                for (int i = 0; i < s.num_bands; ++i) {
                    const auto v = static_cast <int>(br.get_bits(8));
                    quant[i] = s.quant_table[std::min(v, 95)];
                }

                int k = 0;
                float q = quant[0];
                int i = 2;
                while (i < s.frame_len) {
                    int j;
                    if (s.version_b) {
                        j = i + 16;
                    } else {
                        if (br.bits_left() < 1u) {
                            return make_unexpected<error_type>("bink: rle flag truncated");
                        }
                        if (br.get_bit() != 0u) {
                            const auto rle = static_cast <int>(br.get_bits(4));
                            j = i + kRleLengthTab[rle] * 8;
                        } else {
                            j = i + 8;
                        }
                    }
                    j = std::min(j, s.frame_len);

                    if (br.bits_left() < 4u) {
                        return make_unexpected<error_type>("bink: width bits truncated");
                    }
                    const auto width = static_cast <int>(br.get_bits(4));
                    if (width == 0) {
                        for (int k2 = i; k2 < j; ++k2) coeffs[k2] = 0.0f;
                        i = j;
                        while (k < s.num_bands &&
                               s.bands[k] < static_cast <unsigned int>(i)) {
                            q = quant[k++];
                        }
                    } else {
                        while (i < j) {
                            if (k < s.num_bands &&
                                s.bands[k] == static_cast <unsigned int>(i)) {
                                q = quant[k++];
                            }
                            if (br.bits_left() < static_cast <std::size_t>(width)) {
                                return make_unexpected<error_type>(
                                    "bink: coeff bits truncated");
                            }
                            const auto coeff = static_cast <int>(
                                br.get_bits(static_cast <unsigned int>(width)));
                            if (coeff != 0) {
                                if (br.bits_left() < 1u) {
                                    return make_unexpected<error_type>(
                                        "bink: coeff sign truncated");
                                }
                                if (br.get_bit() != 0u)
                                    coeffs[i] = -q * static_cast <float>(coeff);
                                else
                                    coeffs[i] =  q * static_cast <float>(coeff);
                            } else {
                                coeffs[i] = 0.0f;
                            }
                            ++i;
                        }
                    }
                }

                std::vector <float> out_samples(
                    static_cast <std::size_t>(s.frame_len), 0.0f);

                if (s.use_dct) {
                    coeffs[0] /= 0.5f;
                    // Run inverse DCT in-place over the first frame_len/2
                    // coefficients, then promote to a frame_len-long
                    // output via type-III's natural symmetry... actually
                    // ffmpeg's DCT-III of length N returns N real
                    // samples (the time-domain output).
                    // pocketfft's dct type 3 with shape={N/2} produces
                    // N/2 outputs. ffmpeg's frame_len here is N (the
                    // PCM-domain length), but `av_tx_init` was called
                    // with size 1<<(frame_len_bits-1) = N/2. So the
                    // DCT operates on N/2 input → N/2 output and the
                    // output goes into the first half of out_samples.
                    inverse_dct(coeffs.data(),
                                static_cast <std::size_t>(s.frame_len / 2));
                    std::memcpy(out_samples.data(), coeffs.data(),
                                static_cast <std::size_t>(s.frame_len / 2) * sizeof(float));
                } else {
                    // ffmpeg conjugates the imaginary parts before the
                    // c2r — effectively flipping the transform direction
                    // sign. Mirror that.
                    for (int ii = 2; ii < s.frame_len; ii += 2) {
                        coeffs[ii + 1] = -coeffs[ii + 1];
                    }
                    coeffs[s.frame_len + 0] = coeffs[1];
                    coeffs[s.frame_len + 1] = 0.0f;
                    coeffs[1] = 0.0f;
                    inverse_rdft(coeffs.data(),
                                 static_cast <std::size_t>(s.frame_len));
                    std::memcpy(out_samples.data(), coeffs.data(),
                                static_cast <std::size_t>(s.frame_len) * sizeof(float));
                }

                // Overlap-add against this channel's `previous` tail.
                const std::size_t abs_ch =
                    static_cast <std::size_t>(s.ch_offset + ch);
                if (!s.first) {
                    const float count_f =
                        static_cast <float>(s.overlap_len * local_channels);
                    int jj = ch;
                    for (int ii = 0; ii < s.overlap_len; ++ii, jj += local_channels) {
                        const std::size_t idx = static_cast <std::size_t>(ii);
                        const float prev = s.previous[abs_ch][idx];
                        const float cur  = out_samples[idx];
                        out_samples[idx] =
                            (prev * static_cast <float>(
                                static_cast <int>(count_f) - jj) +
                             cur  * static_cast <float>(jj)) /
                            count_f;
                    }
                }
                std::memcpy(s.previous[abs_ch].data(),
                            out_samples.data() +
                                static_cast <std::size_t>(s.frame_len - s.overlap_len),
                            static_cast <std::size_t>(s.overlap_len) * sizeof(float));

                // Append the visible portion (frame_len - overlap_len)
                // to the channel's output accumulator.
                const std::size_t visible =
                    static_cast <std::size_t>(s.frame_len - s.overlap_len);
                auto& acc = per_channel_acc[abs_ch];
                acc.insert(acc.end(), out_samples.begin(),
                           out_samples.begin() + static_cast <std::ptrdiff_t>(visible));
            }
            return {};
        }
    } // namespace

    result audio_decode_packet(audio_decoder& s,
                               std::span <const std::uint8_t> packet,
                               std::vector <float>& out) {
        if (packet.size() < 4u) {
            return make_unexpected<error_type>("bink: audio packet too short");
        }
        // The first 4 bytes are a "reported size" hint that we skip per
        // ffmpeg's binkaudio_receive_frame.
        bit_reader br{packet};
        br.skip_bits(32);

        // Per-channel intermediate buffers; we'll interleave at the end.
        const int total_channels = s.use_dct ? s.channels : 1;
        std::vector <std::vector <float>> per_channel_acc(
            static_cast <std::size_t>(total_channels));

        s.ch_offset = 0;
        while (s.ch_offset < total_channels) {
            const int local =
                std::min(kAudioMaxChannels, total_channels - s.ch_offset);
            if (auto r = decode_block(s, br, local, per_channel_acc); !r) {
                return r;
            }
            s.ch_offset += kAudioMaxChannels;
            // Align bit cursor to next 32-bit boundary between blocks.
            const auto pos = br.bits_consumed();
            const auto pad = (32u - (pos & 31u)) & 31u;
            if (pad) br.skip_bits(static_cast <unsigned int>(pad));
            if (br.bits_left() == 0u) break;
        }
        s.first = false;

        // Interleave (or keep mono) into `out`. For RDFT (single-channel
        // internal stream representing all channels interleaved already)
        // the data is already in the right order. For DCT, interleave
        // per-channel buffers.
        if (s.use_dct) {
            // All channels should have produced the same number of
            // samples. Interleave.
            std::size_t per_channel = per_channel_acc.empty()
                ? 0
                : per_channel_acc[0].size();
            for (const auto& v : per_channel_acc) {
                if (v.size() != per_channel) {
                    return make_unexpected<error_type>(
                        "bink: per-channel sample count mismatch");
                }
            }
            const auto base = out.size();
            out.resize(base + per_channel * static_cast <std::size_t>(total_channels));
            for (std::size_t i = 0; i < per_channel; ++i) {
                for (std::size_t ch = 0;
                     ch < static_cast <std::size_t>(total_channels); ++ch) {
                    out[base + i * static_cast <std::size_t>(total_channels) + ch] =
                        per_channel_acc[ch][i];
                }
            }
        } else {
            // RDFT path keeps the single-stream output (already
            // channel-interleaved at the bit-stream level).
            out.insert(out.end(), per_channel_acc[0].begin(), per_channel_acc[0].end());
        }
        return {};
    }
} // namespace bink
