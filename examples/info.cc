#include <onyx_anim/codecs/register_codecs.hh>
#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/probe.hh>

#include <musac/sdk/io_stream.hh>

#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <animation file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    auto stream = musac::io_from_file(argv[1], "rb");
    if (!stream) {
        std::fprintf(stderr, "Cannot open: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    onyx_anim::register_all_codecs(onyx_anim::codec_registry::instance());

    auto pr = onyx_anim::probe(stream.get());
    if (!pr) {
        std::fprintf(stderr, "%s: %s\n", argv[1], pr.error().c_str());
        return EXIT_FAILURE;
    }

    std::printf("codec: %s\n", pr->codec_name.c_str());
    std::printf("size:    %ux%u\n", pr->video.width, pr->video.height);
    std::printf("frames:  %u\n", pr->video.frame_count);
    {
        const long long us  = static_cast<long long>(pr->video.frame_period.count());
        const double    fps = us > 0 ? 1'000'000.0 / static_cast<double>(us) : 0.0;
        std::printf("period:  %lld us  (%.2f fps)\n", us, fps);
    }
    if (pr->audio_tracks.empty()) {
        std::printf("audio:   none\n");
    } else {
        for (std::size_t i = 0; i < pr->audio_tracks.size(); ++i) {
            const auto& t = pr->audio_tracks[i];
            std::printf("audio[%zu]: %u Hz, %u ch, %u-bit  %s\n",
                        i, t.sample_rate, t.channels, t.bits_per_sample,
                        t.codec_name);
        }
    }
    return EXIT_SUCCESS;
}
