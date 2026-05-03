// Decode an animation file and dump the first audio track to a raw float
// PCM file (interleaved). Used to compare bink audio output against
// ffmpeg's reference for diagnostics.

#include <onyx_anim/codecs/register_codecs.hh>
#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <musac/audio_source.hh>
#include <musac/sdk/decoder.hh>
#include <musac/sdk/io_stream.hh>

#include <cstdio>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <input> <output.f32>\n", argv[0]);
        return 2;
    }
    onyx_anim::register_all_codecs(onyx_anim::codec_registry::instance());

    auto stream = musac::io_from_file(argv[1], "rb");
    if (!stream) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    auto& reg = onyx_anim::codec_registry::instance();
    auto dec = reg.create_decoder(stream.get());
    if (!dec) { std::fprintf(stderr, "no codec for %s\n", argv[1]); return 2; }
    auto opened = dec->open(stream.get());
    if (!opened) {
        std::fprintf(stderr, "open failed: %s\n", opened.error().c_str());
        return 2;
    }
    if (dec->audio_track_count() == 0) {
        std::fprintf(stderr, "no audio\n");
        return 2;
    }
    auto track = dec->take_audio_track(0);
    if (!track) { std::fprintf(stderr, "take_audio_track failed\n"); return 2; }

    // Drive the source via its public open/read_samples API. We pick a
    // device-side rate equal to the source rate (no resampling) and
    // 2-channel device output to capture both L and R for stereo.
    constexpr unsigned int kDeviceRate = 44100;
    constexpr unsigned int kFrameSize = 4096;
    track->open(kDeviceRate, 2, kFrameSize);

    FILE* f = std::fopen(argv[2], "wb");
    if (!f) { std::fprintf(stderr, "cannot open output\n"); return 2; }

    std::vector<float> buf(kFrameSize * 2, 0.0f);
    std::size_t cur_pos = 0;
    std::size_t total_written = 0;
    // Drain until silence + EOF. We drain a fixed cap to bound runtime.
    constexpr std::size_t kMaxFrames = 6000;
    for (std::size_t frame = 0; frame < kMaxFrames; ++frame) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        cur_pos = 0;
        track->read_samples(buf.data(), cur_pos, buf.size(), 2);
        if (cur_pos == 0) break;
        std::fwrite(buf.data(), sizeof(float), cur_pos, f);
        total_written += cur_pos;
    }
    std::fclose(f);
    std::fprintf(stderr, "wrote %zu float samples (rate=%u ch=2)\n",
                 total_written, kDeviceRate);
    return 0;
}
