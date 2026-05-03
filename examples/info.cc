#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>
#include <onyx_anim/sdk/types.hh>
#include <onyx_anim/codecs/register_codecs.hh>

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

    auto& reg = onyx_anim::codec_registry::instance();
    onyx_anim::register_all_codecs(reg);

    auto dec = reg.create_decoder(stream.get());
    if (!dec) {
        std::fprintf(stderr, "No decoder accepted: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    std::printf("codec: %.*s\n",
                static_cast<int>(dec->name().size()), dec->name().data());

    if (auto r = dec->open(stream.get()); !r) {
        std::fprintf(stderr, "open failed: %s\n", r.error().c_str());
        return EXIT_FAILURE;
    }

    const auto& info = dec->info();
    std::printf("size:    %ux%u\n", info.width, info.height);
    std::printf("frames:  %u\n", info.frame_count);
    {
        const long long us  = static_cast<long long>(info.frame_period.count());
        const double    fps = us > 0 ? 1'000'000.0 / static_cast<double>(us) : 0.0;
        std::printf("period:  %lld us  (%.2f fps)\n", us, fps);
    }
    std::printf("audio:   %u track(s) @ %u Hz, %u ch\n",
                info.audio_track_count, info.audio_rate, info.audio_channels);
    return EXIT_SUCCESS;
}
