// flc_to_ppm — decode an FLC/FLI animation through the public onyx_anim API
// and write each frame as a binary PPM (P6) into an output directory.
//
// Usage:
//   flc_to_ppm <input.flc> <output_dir>
//
// Frames are written as `frame_NNNN.ppm` starting from index 0. The output
// directory must already exist.
//
// Used by scripts/flc_cross_check.sh to bit-compare against ffmpeg.

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>
#include <onyx_anim/codecs/register_codecs.hh>

#include <onyx_image/surface.hpp>
#include <musac/sdk/io_stream.hh>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

bool write_ppm(const std::filesystem::path& path,
               unsigned int width, unsigned int height,
               std::span<const std::uint8_t> indexed,
               std::span<const std::uint8_t> palette /* 768 bytes */) {
    std::FILE* fp = std::fopen(path.string().c_str(), "wb");
    if (!fp) return false;

    // P6 header
    std::fprintf(fp, "P6\n%u %u\n255\n", width, height);

    // Convert indexed8 + palette to interleaved RGB. Allocate one row at a time
    // to keep memory bounded for large frames.
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3u);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t idx = indexed[y * width + x];
            row[x * 3 + 0] = palette[idx * 3 + 0];
            row[x * 3 + 1] = palette[idx * 3 + 1];
            row[x * 3 + 2] = palette[idx * 3 + 2];
        }
        if (std::fwrite(row.data(), 1, row.size(), fp) != row.size()) {
            std::fclose(fp);
            return false;
        }
    }
    return std::fclose(fp) == 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <input.flc> <output_dir>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const std::filesystem::path input_path  = argv[1];
    const std::filesystem::path output_dir  = argv[2];

    if (!std::filesystem::is_directory(output_dir)) {
        std::fprintf(stderr, "output directory does not exist: %s\n",
                     output_dir.string().c_str());
        return EXIT_FAILURE;
    }

    auto stream = musac::io_from_file(input_path.string().c_str(), "rb");
    if (!stream) {
        std::fprintf(stderr, "cannot open input: %s\n", input_path.string().c_str());
        return EXIT_FAILURE;
    }

    auto& reg = onyx_anim::codec_registry::instance();
    onyx_anim::register_all_codecs(reg);

    auto dec = reg.create_decoder(stream.get());
    if (!dec) {
        std::fprintf(stderr, "no decoder accepted: %s\n", input_path.string().c_str());
        return EXIT_FAILURE;
    }

    if (auto r = dec->open(stream.get()); !r) {
        std::fprintf(stderr, "open failed: %s\n", r.error().c_str());
        return EXIT_FAILURE;
    }

    const auto& info = dec->info();
    if (info.format != onyx_anim::pixel_format::indexed8) {
        std::fprintf(stderr, "expected indexed8 surface, got format %d\n",
                     static_cast<int>(info.format));
        return EXIT_FAILURE;
    }

    onyx_image::memory_surface surf;
    unsigned int idx = 0;
    while (!dec->eof()) {
        auto fr = dec->decode_frame(surf);
        if (!fr) {
            std::fprintf(stderr, "frame %u decode failed: %s\n",
                         idx, fr.error().c_str());
            return EXIT_FAILURE;
        }

        char name[32];
        std::snprintf(name, sizeof(name), "frame_%04u.ppm", idx);
        const auto out = output_dir / name;

        if (!write_ppm(out,
                       static_cast<unsigned int>(surf.width()),
                       static_cast<unsigned int>(surf.height()),
                       surf.pixels(), surf.palette())) {
            std::fprintf(stderr, "failed to write %s\n", out.string().c_str());
            return EXIT_FAILURE;
        }
        ++idx;
    }

    std::printf("%u frames written to %s\n", idx, output_dir.string().c_str());
    return EXIT_SUCCESS;
}
