#include <onyx_anim/codecs/register_codecs.hh>
#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

#include <flc/chunks.hh>
#include <flc/decoders.hh>
#include <flc/header.hh>

#include <musac/sdk/io_stream.hh>
#include <onyx_image/surface.hpp>

#include <nanobench.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct config {
    std::vector<std::filesystem::path> inputs;
    onyx_anim::decode_options decode_opts{};
    std::string unit = "frame";
    std::string filter;
    std::filesystem::path json_path;
    std::filesystem::path csv_path;
    std::filesystem::path html_path;
    std::size_t epochs = 11;
    std::uint64_t warmup = 1;
    std::uint64_t max_frames = 0;
    int min_epoch_ms = 1;
    int max_epoch_ms = 100;
    bool performance_counters = true;
    bool quiet = false;
    bool allow_unoptimized = false;
    bool chunk_core = false;
};

struct prepared_decoder {
    std::unique_ptr<musac::io_stream> stream;
    std::unique_ptr<onyx_anim::anim_decoder> decoder;
};

struct input_case {
    std::filesystem::path path;
    std::string codec;
    prepared_decoder prepared;
    std::string chunk_summary;
};

struct chunk_counter {
    std::uint64_t count = 0;
    std::uint64_t payload_bytes = 0;
};

struct flc_chunk_report {
    std::uint64_t frames = 0;
    std::uint64_t chunks = 0;
    std::map<std::string, chunk_counter> by_type;
    std::string error;
};

struct flc_core_chunks {
    unsigned int width = 0;
    unsigned int height = 0;
    std::vector<std::vector<std::uint8_t>> lc;
    std::vector<std::vector<std::uint8_t>> ss2;
    std::string error;
};

enum class flc_core_op : std::uint8_t {
    literal,
    fill_byte,
    fill_word,
    store_byte,
};

struct flc_core_command {
    flc_core_op op = flc_core_op::literal;
    std::uint32_t dst = 0;
    std::uint32_t count = 0;
    std::uint32_t payload = 0;
    std::uint32_t src = 0;
    std::uint8_t lo = 0;
    std::uint8_t hi = 0;
};

struct flc_compiled_core {
    unsigned int width = 0;
    unsigned int height = 0;
    std::vector<flc_core_command> lc;
    std::vector<flc_core_command> ss2;
    std::string error;
};

[[noreturn]] void fail(const std::string& msg) {
    std::fprintf(stderr, "bench_decode: %s\n", msg.c_str());
    std::exit(EXIT_FAILURE);
}

void usage(const char* argv0) {
    std::printf(
        "Usage: %s [options] <animation>...\n"
        "\n"
        "Options:\n"
        "  --format=<rgb888|rgba8888|indexed8>  Preferred decoder output format (default: rgb888)\n"
        "  --unit=<frame|pixel>                 Report decode throughput per frame or pixel (default: frame)\n"
        "  --epochs=<n>                         Nanobench measurement epochs (default: 11)\n"
        "  --warmup=<n>                         Warmup iterations per benchmark (default: 1)\n"
        "  --max-frames=<n>                     Decode at most n frames per iteration (default: all)\n"
        "  --chunk-core                         Also benchmark FLI/FLC LC and SS2 chunk decoders directly\n"
        "  --min-epoch-ms=<n>                   Minimum epoch time in ms (default: 1)\n"
        "  --max-epoch-ms=<n>                   Maximum epoch time in ms (default: 100)\n"
        "  --filter=<regex>                     Run benchmark names matching regex\n"
        "  --json=<path>                        Write nanobench JSON results\n"
        "  --csv=<path>                         Write nanobench CSV results\n"
        "  --html=<path>                        Write nanobench HTML boxplot results\n"
        "  --no-counters                        Disable hardware performance counters\n"
        "  --no-frame-index                     Open decoders without eager frame indexing\n"
        "  --quiet                              Suppress nanobench markdown output\n"
        "  --allow-unoptimized                  Allow running without NDEBUG\n"
        "  --help                               Show this help\n",
        argv0);
}

bool consume_prefix(std::string_view arg, std::string_view prefix, std::string& out) {
    if (!arg.starts_with(prefix)) return false;
    out.assign(arg.substr(prefix.size()));
    return true;
}

std::uint64_t parse_u64(std::string_view text, std::string_view opt_name) {
    try {
        std::size_t consumed = 0;
        auto value = std::stoull(std::string{text}, &consumed, 10);
        if (consumed != text.size()) fail("invalid integer for " + std::string{opt_name});
        return value;
    } catch (const std::exception&) {
        fail("invalid integer for " + std::string{opt_name});
    }
}

int parse_int(std::string_view text, std::string_view opt_name) {
    const auto value = parse_u64(text, opt_name);
    if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        fail("integer out of range for " + std::string{opt_name});
    }
    return static_cast<int>(value);
}

onyx_anim::pixel_format parse_pixel_format(std::string_view value) {
    if (value == "indexed8") return onyx_anim::pixel_format::indexed8;
    if (value == "rgb888") return onyx_anim::pixel_format::rgb888;
    if (value == "rgba8888") return onyx_anim::pixel_format::rgba8888;
    fail("unknown --format value: " + std::string{value});
}

const char* pixel_format_name(onyx_anim::pixel_format fmt) {
    switch (fmt) {
        case onyx_anim::pixel_format::indexed8: return "indexed8";
        case onyx_anim::pixel_format::rgb888: return "rgb888";
        case onyx_anim::pixel_format::rgba8888: return "rgba8888";
    }
    return "unknown";
}

config parse_args(int argc, char** argv) {
    config cfg;
    cfg.decode_opts.preferred_format = onyx_anim::pixel_format::rgb888;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;

        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else if (arg == "--no-counters") {
            cfg.performance_counters = false;
        } else if (arg == "--no-frame-index") {
            cfg.decode_opts.build_frame_index = false;
        } else if (arg == "--quiet") {
            cfg.quiet = true;
        } else if (arg == "--allow-unoptimized") {
            cfg.allow_unoptimized = true;
        } else if (arg == "--chunk-core") {
            cfg.chunk_core = true;
        } else if (consume_prefix(arg, "--format=", value) ||
                   consume_prefix(arg, "--preferred-format=", value)) {
            cfg.decode_opts.preferred_format = parse_pixel_format(value);
        } else if (consume_prefix(arg, "--unit=", value)) {
            if (value != "frame" && value != "pixel") {
                fail("unknown --unit value: " + value);
            }
            cfg.unit = value;
        } else if (consume_prefix(arg, "--epochs=", value)) {
            cfg.epochs = static_cast<std::size_t>(parse_u64(value, "--epochs"));
            if (cfg.epochs == 0) fail("--epochs must be greater than zero");
        } else if (consume_prefix(arg, "--warmup=", value)) {
            cfg.warmup = parse_u64(value, "--warmup");
        } else if (consume_prefix(arg, "--max-frames=", value)) {
            cfg.max_frames = parse_u64(value, "--max-frames");
        } else if (consume_prefix(arg, "--min-epoch-ms=", value)) {
            cfg.min_epoch_ms = parse_int(value, "--min-epoch-ms");
        } else if (consume_prefix(arg, "--max-epoch-ms=", value)) {
            cfg.max_epoch_ms = parse_int(value, "--max-epoch-ms");
        } else if (consume_prefix(arg, "--filter=", value)) {
            cfg.filter = value;
        } else if (consume_prefix(arg, "--json=", value)) {
            cfg.json_path = value;
        } else if (consume_prefix(arg, "--csv=", value)) {
            cfg.csv_path = value;
        } else if (consume_prefix(arg, "--html=", value)) {
            cfg.html_path = value;
        } else if (!arg.empty() && arg[0] == '-') {
            fail("unknown option: " + arg);
        } else {
            cfg.inputs.emplace_back(arg);
        }
    }

    if (cfg.inputs.empty()) fail("no input files provided");
    if (cfg.min_epoch_ms < 0 || cfg.max_epoch_ms < 0 ||
        cfg.min_epoch_ms > cfg.max_epoch_ms) {
        fail("invalid epoch time bounds");
    }
    return cfg;
}

void register_codecs_once() {
    static bool registered = false;
    if (!registered) {
        onyx_anim::register_all_codecs(onyx_anim::codec_registry::instance());
        registered = true;
    }
}

prepared_decoder open_decoder(const std::filesystem::path& path,
                              const onyx_anim::decode_options& opts) {
    prepared_decoder prepared;
    prepared.stream = musac::io_from_file(path.string().c_str(), "rb");
    if (!prepared.stream) fail("cannot open input: " + path.string());

    auto& reg = onyx_anim::codec_registry::instance();
    prepared.decoder = reg.create_decoder(prepared.stream.get());
    if (!prepared.decoder) fail("no decoder accepted: " + path.string());

    if (auto r = prepared.decoder->open(prepared.stream.get(), opts); !r) {
        fail("open failed for " + path.string() + ": " + r.error());
    }
    return prepared;
}

std::uint64_t frame_batch(const onyx_anim::anim_info& info, std::uint64_t max_frames) {
    auto frames = static_cast<std::uint64_t>(info.frame_count);
    if (frames == 0) frames = 1;
    if (max_frames != 0) frames = std::min(frames, max_frames);
    return std::max<std::uint64_t>(frames, 1);
}

std::uint64_t unit_batch(const onyx_anim::anim_info& info,
                         std::uint64_t max_frames,
                         const std::string& unit) {
    const auto frames = frame_batch(info, max_frames);
    if (unit == "frame") return frames;
    const auto pixels = static_cast<std::uint64_t>(info.width) *
                        static_cast<std::uint64_t>(info.height);
    return std::max<std::uint64_t>(frames * std::max<std::uint64_t>(pixels, 1), 1);
}

std::string bench_name(std::string_view kind,
                       const std::filesystem::path& path,
                       std::string_view codec) {
    return std::string{kind} + "/" + std::string{codec} + "/" +
           path.filename().string();
}

std::string sub_chunk_name(flc::sub_chunk_type type) {
    switch (type) {
        case flc::sub_chunk_type::color_256: return "color_256";
        case flc::sub_chunk_type::ss2: return "ss2";
        case flc::sub_chunk_type::color_64: return "color_64";
        case flc::sub_chunk_type::lc: return "lc";
        case flc::sub_chunk_type::black: return "black";
        case flc::sub_chunk_type::brun: return "brun";
        case flc::sub_chunk_type::copy: return "copy";
        case flc::sub_chunk_type::pstamp: return "pstamp";
    }
    return "type_" + std::to_string(static_cast<std::uint16_t>(type));
}

std::string format_bytes(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull) {
        return std::to_string(bytes / (1024ull * 1024ull)) + "MiB";
    }
    if (bytes >= 1024ull) {
        return std::to_string(bytes / 1024ull) + "KiB";
    }
    return std::to_string(bytes) + "B";
}

std::optional<flc_chunk_report> scan_flc_chunks(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (bytes.size() < flc::kFileHeaderSize) return std::nullopt;

    auto header = flc::parse_file_header(std::span<const std::uint8_t>{
        bytes.data(), bytes.size()});
    if (!header) return std::nullopt;

    flc_chunk_report report;
    std::size_t off = flc::kFileHeaderSize;
    while (off + 6u <= bytes.size() && report.frames < header->frame_count) {
        const std::uint32_t chunk_size = bytes::read_u32le(bytes.data() + off);
        const std::uint16_t chunk_magic = bytes::read_u16le(bytes.data() + off + 4u);
        if (chunk_size < 6u) {
            report.error = "top-level chunk size smaller than header";
            break;
        }
        if (off + chunk_size > bytes.size()) {
            report.error = "top-level chunk truncated";
            break;
        }

        const bool is_frame = chunk_magic == flc::kFrameMagicStandard ||
                              chunk_magic == flc::kFrameMagicVariant;
        if (is_frame) {
            auto frame = flc::parse_frame_header(std::span<const std::uint8_t>{
                bytes.data() + off, chunk_size});
            if (!frame) {
                report.error = frame.error();
                break;
            }

            ++report.frames;
            std::size_t sub_off = off + flc::kFrameHeaderSize;
            const std::size_t frame_end = off + chunk_size;
            for (unsigned int sc = 0; sc < frame->sub_chunks; ++sc) {
                if (sub_off + flc::kSubChunkHeaderSize > frame_end) {
                    report.error = "sub-chunk header truncated";
                    break;
                }
                auto sub = flc::parse_sub_chunk_header(std::span<const std::uint8_t>{
                    bytes.data() + sub_off, frame_end - sub_off});
                if (!sub) {
                    report.error = sub.error();
                    break;
                }
                if (sub_off + sub->size > frame_end) {
                    report.error = "sub-chunk payload truncated";
                    break;
                }

                auto& counter = report.by_type[sub_chunk_name(sub->type)];
                ++counter.count;
                counter.payload_bytes += sub->size - flc::kSubChunkHeaderSize;
                ++report.chunks;
                sub_off += sub->size;
            }
            if (!report.error.empty()) break;
        }

        off += chunk_size;
    }

    return report;
}

std::optional<flc_core_chunks> collect_flc_core_chunks(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (bytes.size() < flc::kFileHeaderSize) return std::nullopt;

    auto header = flc::parse_file_header(std::span<const std::uint8_t>{
        bytes.data(), bytes.size()});
    if (!header) return std::nullopt;

    flc_core_chunks out;
    out.width = header->width;
    out.height = header->height;

    std::uint64_t frames = 0;
    std::size_t off = flc::kFileHeaderSize;
    while (off + 6u <= bytes.size() && frames < header->frame_count) {
        const std::uint32_t chunk_size = bytes::read_u32le(bytes.data() + off);
        const std::uint16_t chunk_magic = bytes::read_u16le(bytes.data() + off + 4u);
        if (chunk_size < 6u) {
            out.error = "top-level chunk size smaller than header";
            break;
        }
        if (off + chunk_size > bytes.size()) {
            out.error = "top-level chunk truncated";
            break;
        }

        const bool is_frame = chunk_magic == flc::kFrameMagicStandard ||
                              chunk_magic == flc::kFrameMagicVariant;
        if (is_frame) {
            auto frame = flc::parse_frame_header(std::span<const std::uint8_t>{
                bytes.data() + off, chunk_size});
            if (!frame) {
                out.error = frame.error();
                break;
            }

            ++frames;
            std::size_t sub_off = off + flc::kFrameHeaderSize;
            const std::size_t frame_end = off + chunk_size;
            for (unsigned int sc = 0; sc < frame->sub_chunks; ++sc) {
                if (sub_off + flc::kSubChunkHeaderSize > frame_end) {
                    out.error = "sub-chunk header truncated";
                    break;
                }
                auto sub = flc::parse_sub_chunk_header(std::span<const std::uint8_t>{
                    bytes.data() + sub_off, frame_end - sub_off});
                if (!sub) {
                    out.error = sub.error();
                    break;
                }
                if (sub_off + sub->size > frame_end) {
                    out.error = "sub-chunk payload truncated";
                    break;
                }

                const auto payload_begin = sub_off + flc::kSubChunkHeaderSize;
                const auto payload_end = sub_off + sub->size;
                if (sub->type == flc::sub_chunk_type::lc) {
                    out.lc.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(payload_begin),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(payload_end));
                } else if (sub->type == flc::sub_chunk_type::ss2) {
                    out.ss2.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(payload_begin),
                                         bytes.begin() + static_cast<std::ptrdiff_t>(payload_end));
                }
                sub_off += sub->size;
            }
            if (!out.error.empty()) break;
        }

        off += chunk_size;
    }

    return out;
}

bool compile_lc_payload(const std::vector<std::uint8_t>& payload,
                        std::uint32_t payload_index,
                        unsigned int width,
                        unsigned int height,
                        std::vector<flc_core_command>& commands,
                        std::string& error) {
    const std::uint8_t* p = payload.data();
    const std::uint8_t* const end = payload.data() + payload.size();
    const auto has = [&p, end](std::size_t n) noexcept {
        return static_cast<std::size_t>(end - p) >= n;
    };

    if (!has(4)) {
        error = "LC truncated at chunk header";
        return false;
    }
    const auto line_skip = static_cast<unsigned int>(bytes::read_u16le(p));
    const auto line_count = static_cast<unsigned int>(bytes::read_u16le(p + 2));
    p += 4;

    if (line_skip + line_count > height) {
        error = "LC line range exceeds frame height";
        return false;
    }

    for (unsigned int li = 0; li < line_count; ++li) {
        const unsigned int y = line_skip + li;
        unsigned int x = 0;
        if (!has(1)) {
            error = "LC truncated at packet count";
            return false;
        }
        const auto packet_count = *p++;

        for (unsigned int packet = 0; packet < packet_count; ++packet) {
            if (!has(2)) {
                error = "LC truncated at packet header";
                return false;
            }
            const auto col_skip = static_cast<unsigned int>(p[0]);
            const auto rle_count = static_cast<std::int8_t>(p[1]);
            p += 2;

            x += col_skip;
            if (x > width) {
                error = "LC column skip overflows row";
                return false;
            }
            if (rle_count == 0) continue;

            const auto dst = static_cast<std::uint32_t>(y * width + x);
            if (rle_count > 0) {
                const auto n = static_cast<unsigned int>(rle_count);
                if (!has(n)) {
                    error = "LC literal run truncated";
                    return false;
                }
                if (n > width - x) {
                    error = "LC literal run overflows row";
                    return false;
                }
                commands.push_back({flc_core_op::literal,
                                    dst,
                                    static_cast<std::uint32_t>(n),
                                    payload_index,
                                    static_cast<std::uint32_t>(p - payload.data()),
                                    0,
                                    0});
                p += n;
                x += n;
            } else {
                const auto n = static_cast<unsigned int>(-static_cast<int>(rle_count));
                if (!has(1)) {
                    error = "LC truncated at replicate byte";
                    return false;
                }
                if (n > width - x) {
                    error = "LC replicate run overflows row";
                    return false;
                }
                commands.push_back({flc_core_op::fill_byte,
                                    dst,
                                    static_cast<std::uint32_t>(n),
                                    payload_index,
                                    0,
                                    *p++,
                                    0});
                x += n;
            }
        }
    }
    return true;
}

bool compile_ss2_payload(const std::vector<std::uint8_t>& payload,
                         std::uint32_t payload_index,
                         unsigned int width,
                         unsigned int height,
                         std::vector<flc_core_command>& commands,
                         std::string& error) {
    const std::uint8_t* p = payload.data();
    const std::uint8_t* const end = payload.data() + payload.size();
    const auto has = [&p, end](std::size_t n) noexcept {
        return static_cast<std::size_t>(end - p) >= n;
    };

    if (!has(2)) {
        error = "SS2 truncated at line_count";
        return false;
    }
    const auto line_count = static_cast<unsigned int>(bytes::read_u16le(p));
    p += 2;

    unsigned int y = 0;
    for (unsigned int li = 0; li < line_count; ++li) {
        if (!has(2)) {
            error = "SS2 truncated at line opcode";
            return false;
        }
        std::uint16_t opcode = bytes::read_u16le(p);
        p += 2;
        std::uint16_t packet_count = opcode;

        if ((opcode & 0xC000u) != 0) {
            for (;;) {
                const unsigned tag = (opcode >> 14) & 0x3u;
                if (tag == 0u) {
                    packet_count = opcode;
                    break;
                }
                if (tag == 0b11u) {
                    const auto signed_op = static_cast<std::int16_t>(opcode);
                    y += static_cast<unsigned int>(-signed_op);
                    if (y > height) {
                        error = "SS2 line skip overflows height";
                        return false;
                    }
                } else if (tag == 0b10u) {
                    if (y >= height || width == 0) {
                        error = "SS2 last-byte on invalid row";
                        return false;
                    }
                    commands.push_back({flc_core_op::store_byte,
                                        static_cast<std::uint32_t>(y * width + (width - 1u)),
                                        1,
                                        payload_index,
                                        0,
                                        static_cast<std::uint8_t>(opcode & 0xFFu),
                                        0});
                } else {
                    error = "SS2 undefined opcode tag";
                    return false;
                }

                if (!has(2)) {
                    error = "SS2 truncated at line opcode";
                    return false;
                }
                opcode = bytes::read_u16le(p);
                p += 2;
            }
        }

        if (y >= height) {
            error = "SS2 packets on out-of-range row";
            return false;
        }

        unsigned int x = 0;
        for (unsigned int packet = 0; packet < packet_count; ++packet) {
            if (!has(2)) {
                error = "SS2 truncated at packet header";
                return false;
            }
            const auto col_skip = static_cast<unsigned int>(p[0]);
            const auto rle_count = static_cast<std::int8_t>(p[1]);
            p += 2;

            x += col_skip;
            if (x > width) {
                error = "SS2 column skip overflows row";
                return false;
            }
            if (rle_count == 0) continue;

            const auto dst = static_cast<std::uint32_t>(y * width + x);
            if (rle_count > 0) {
                const auto n_words = static_cast<unsigned int>(rle_count);
                const auto n_bytes = n_words * 2u;
                if (!has(n_bytes)) {
                    error = "SS2 literal run truncated";
                    return false;
                }
                if (n_bytes > width - x) {
                    error = "SS2 literal run overflows row";
                    return false;
                }
                commands.push_back({flc_core_op::literal,
                                    dst,
                                    static_cast<std::uint32_t>(n_bytes),
                                    payload_index,
                                    static_cast<std::uint32_t>(p - payload.data()),
                                    0,
                                    0});
                p += n_bytes;
                x += n_bytes;
            } else {
                const auto n_words = static_cast<unsigned int>(-static_cast<int>(rle_count));
                const auto n_bytes = n_words * 2u;
                if (!has(2)) {
                    error = "SS2 truncated at replicate word";
                    return false;
                }
                if (n_bytes > width - x) {
                    error = "SS2 replicate run overflows row";
                    return false;
                }
                const std::uint8_t lo = p[0];
                const std::uint8_t hi = p[1];
                p += 2;
                commands.push_back({lo == hi ? flc_core_op::fill_byte : flc_core_op::fill_word,
                                    dst,
                                    lo == hi ? static_cast<std::uint32_t>(n_bytes)
                                             : static_cast<std::uint32_t>(n_words),
                                    payload_index,
                                    0,
                                    lo,
                                    hi});
                x += n_bytes;
            }
        }
        ++y;
    }
    return true;
}

flc_compiled_core compile_flc_core_chunks(const flc_core_chunks& chunks) {
    flc_compiled_core out;
    out.width = chunks.width;
    out.height = chunks.height;

    for (std::uint32_t i = 0; i < chunks.lc.size(); ++i) {
        if (!compile_lc_payload(chunks.lc[i], i, chunks.width, chunks.height,
                                out.lc, out.error)) {
            return out;
        }
    }
    for (std::uint32_t i = 0; i < chunks.ss2.size(); ++i) {
        if (!compile_ss2_payload(chunks.ss2[i], i, chunks.width, chunks.height,
                                 out.ss2, out.error)) {
            return out;
        }
    }
    return out;
}

std::string chunk_summary(const flc_chunk_report& report) {
    std::vector<std::pair<std::string, chunk_counter>> types(report.by_type.begin(),
                                                             report.by_type.end());
    std::sort(types.begin(), types.end(), [](const auto& a, const auto& b) {
        if (a.second.payload_bytes != b.second.payload_bytes) {
            return a.second.payload_bytes > b.second.payload_bytes;
        }
        return a.second.count > b.second.count;
    });

    std::string out = "frames=" + std::to_string(report.frames) +
                      ", chunks=" + std::to_string(report.chunks);
    const std::size_t limit = std::min<std::size_t>(types.size(), 4);
    for (std::size_t i = 0; i < limit; ++i) {
        out += ", " + types[i].first + "=" +
               std::to_string(types[i].second.count) + "/" +
               format_bytes(types[i].second.payload_bytes);
    }
    if (!report.error.empty()) {
        out += ", scan_error=" + report.error;
    }
    return out;
}

void print_chunk_reports(const std::vector<input_case>& inputs) {
    bool any = false;
    for (const auto& input : inputs) {
        any = any || !input.chunk_summary.empty();
    }
    if (!any) return;

    std::cout << "\nFLI/FLC chunk summary\n";
    for (const auto& input : inputs) {
        if (input.chunk_summary.empty()) continue;
        std::cout << "  " << input.path.filename().string() << ": "
                  << input.chunk_summary << '\n';
    }
    std::cout << '\n';
}

ankerl::nanobench::Bench make_bench(const config& cfg,
                                    std::string_view title,
                                    std::string_view unit) {
    ankerl::nanobench::Bench bench;
    bench.title(std::string{title}.c_str())
         .unit(std::string{unit}.c_str())
         .timeUnit(std::chrono::duration<double, std::milli>(1), "ms")
         .epochs(cfg.epochs)
         .warmup(cfg.warmup)
         .minEpochTime(std::chrono::milliseconds{cfg.min_epoch_ms})
         .maxEpochTime(std::chrono::milliseconds{cfg.max_epoch_ms})
         .performanceCounters(cfg.performance_counters)
         .output(cfg.quiet ? nullptr : &std::cout);
    return bench;
}

void append_results(std::vector<ankerl::nanobench::Result>& all,
                    const ankerl::nanobench::Bench& bench) {
    const auto& results = bench.results();
    all.insert(all.end(), results.begin(), results.end());
}

void render_file(const char* template_text,
                 const std::vector<ankerl::nanobench::Result>& results,
                 const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out) fail("cannot write output: " + path.string());
    ankerl::nanobench::render(template_text, results, out);
}

void run_open_case(const config& cfg,
                   const input_case& item,
                   ankerl::nanobench::Bench& bench) {
    const auto& path = item.path;
    const auto name = bench_name("open", path, item.codec);
    if (!cfg.filter.empty() && !std::regex_search(name, std::regex{cfg.filter})) return;

    bench.batch(1)
         .context("path", path.string().c_str())
         .context("codec", item.codec.c_str())
         .context("chunk_summary", item.chunk_summary.c_str())
         .context("preferred_format", pixel_format_name(cfg.decode_opts.preferred_format))
         .run(name.c_str(), [&] {
             auto prepared = open_decoder(path, cfg.decode_opts);
             ankerl::nanobench::doNotOptimizeAway(prepared.decoder.get());
         });
}

void run_decode_case(const config& cfg,
                     input_case& item,
                     ankerl::nanobench::Bench& bench) {
    auto& prepared = item.prepared;
    const auto& path = item.path;
    const auto& info = prepared.decoder->info();
    const auto name = bench_name("decode", path, item.codec);
    if (!cfg.filter.empty() && !std::regex_search(name, std::regex{cfg.filter})) return;

    bench.batch(unit_batch(info, cfg.max_frames, cfg.unit))
         .context("path", path.string().c_str())
         .context("codec", item.codec.c_str())
         .context("width", std::to_string(info.width).c_str())
         .context("height", std::to_string(info.height).c_str())
         .context("frames", std::to_string(info.frame_count).c_str())
         .context("chunk_summary", item.chunk_summary.c_str())
         .context("preferred_format", pixel_format_name(cfg.decode_opts.preferred_format))
         .context("decoded_format", pixel_format_name(info.format))
         .run(name.c_str(), [&] {
             if (!prepared.decoder->rewind()) {
                 std::fprintf(stderr, "bench_decode: rewind failed for %s\n",
                              path.string().c_str());
                 std::abort();
             }

             onyx_image::memory_surface surf;
             std::uint64_t frames = 0;
             std::uint64_t checksum = 0;
             while (!prepared.decoder->eof() &&
                    (cfg.max_frames == 0 || frames < cfg.max_frames)) {
                 auto fr = prepared.decoder->decode_frame(surf);
                 if (!fr) {
                     std::fprintf(stderr, "bench_decode: decode failed for %s frame %llu: %s\n",
                                  path.string().c_str(),
                                  static_cast<unsigned long long>(frames),
                                  fr.error().c_str());
                     std::abort();
                 }

                 const auto pixels = surf.pixels();
                 if (!pixels.empty()) {
                     checksum += pixels.front();
                     checksum += pixels[pixels.size() / 2u];
                     checksum += pixels.back();
                 }
                 ++frames;
             }

             ankerl::nanobench::doNotOptimizeAway(frames);
             ankerl::nanobench::doNotOptimizeAway(checksum);
         });
}

using flc_chunk_decoder = flc::result (*)(std::span<const std::uint8_t>,
                                          std::uint8_t*,
                                          std::size_t,
                                          unsigned int,
                                          unsigned int);

void run_flc_core_chunk_case(const config& cfg,
                             const input_case& item,
                             const flc_core_chunks& chunks,
                             std::string_view chunk_kind,
                             const std::vector<std::vector<std::uint8_t>>& payloads,
                             flc_chunk_decoder decoder,
                             ankerl::nanobench::Bench& bench) {
    if (payloads.empty()) return;

    const auto name = bench_name("chunk/" + std::string{chunk_kind}, item.path, item.codec);
    if (!cfg.filter.empty() && !std::regex_search(name, std::regex{cfg.filter})) return;

    std::vector<std::uint8_t> fb(static_cast<std::size_t>(chunks.width) *
                                 static_cast<std::size_t>(chunks.height),
                                 std::uint8_t{0});
    const auto payload_bytes = [&payloads] {
        std::uint64_t total = 0;
        for (const auto& payload : payloads) total += payload.size();
        return total;
    }();

    bench.batch(payloads.size())
         .context("path", item.path.string().c_str())
         .context("codec", item.codec.c_str())
         .context("chunk", std::string{chunk_kind}.c_str())
         .context("chunks", std::to_string(payloads.size()).c_str())
         .context("payload_bytes", std::to_string(payload_bytes).c_str())
         .context("width", std::to_string(chunks.width).c_str())
         .context("height", std::to_string(chunks.height).c_str())
         .run(name.c_str(), [&] {
             std::uint64_t checksum = 0;
             for (const auto& payload : payloads) {
                 auto r = decoder(std::span<const std::uint8_t>{payload.data(), payload.size()},
                                  fb.data(), chunks.width, chunks.width, chunks.height);
                 if (!r) {
                     std::fprintf(stderr, "bench_decode: %.*s chunk decode failed for %s: %s\n",
                                  static_cast<int>(chunk_kind.size()),
                                  chunk_kind.data(),
                                  item.path.string().c_str(),
                                  r.error().c_str());
                     std::abort();
                 }
                 if (!fb.empty()) {
                     checksum += fb.front();
                     checksum += fb[fb.size() / 2u];
                     checksum += fb.back();
                 }
             }
             ankerl::nanobench::doNotOptimizeAway(checksum);
         });
}

void execute_compiled_commands(const std::vector<flc_core_command>& commands,
                               const std::vector<std::vector<std::uint8_t>>& payloads,
                               std::uint8_t* fb) {
    for (const auto& command : commands) {
        std::uint8_t* dst = fb + command.dst;
        switch (command.op) {
            case flc_core_op::literal: {
                const auto& payload = payloads[command.payload];
                std::memcpy(dst, payload.data() + command.src, command.count);
                break;
            }
            case flc_core_op::fill_byte:
                std::memset(dst, command.lo, command.count);
                break;
            case flc_core_op::fill_word:
                for (std::uint32_t i = 0; i < command.count; ++i) {
                    dst[i * 2u] = command.lo;
                    dst[i * 2u + 1u] = command.hi;
                }
                break;
            case flc_core_op::store_byte:
                *dst = command.lo;
                break;
        }
    }
}

void run_flc_compiled_chunk_case(const config& cfg,
                                 const input_case& item,
                                 const flc_core_chunks& chunks,
                                 const flc_compiled_core& compiled,
                                 std::string_view chunk_kind,
                                 const std::vector<std::vector<std::uint8_t>>& payloads,
                                 const std::vector<flc_core_command>& commands,
                                 ankerl::nanobench::Bench& bench) {
    if (commands.empty()) return;

    const auto name = bench_name("chunkc/" + std::string{chunk_kind}, item.path, item.codec);
    if (!cfg.filter.empty() && !std::regex_search(name, std::regex{cfg.filter})) return;

    std::vector<std::uint8_t> fb(static_cast<std::size_t>(compiled.width) *
                                 static_cast<std::size_t>(compiled.height),
                                 std::uint8_t{0});
    const auto payload_bytes = [&payloads] {
        std::uint64_t total = 0;
        for (const auto& payload : payloads) total += payload.size();
        return total;
    }();

    bench.batch(payloads.size())
         .context("path", item.path.string().c_str())
         .context("codec", item.codec.c_str())
         .context("chunk", std::string{chunk_kind}.c_str())
         .context("chunks", std::to_string(payloads.size()).c_str())
         .context("commands", std::to_string(commands.size()).c_str())
         .context("payload_bytes", std::to_string(payload_bytes).c_str())
         .context("width", std::to_string(chunks.width).c_str())
         .context("height", std::to_string(chunks.height).c_str())
         .run(name.c_str(), [&] {
             execute_compiled_commands(commands, payloads, fb.data());
             std::uint64_t checksum = 0;
             if (!fb.empty()) {
                 checksum += fb.front();
                 checksum += fb[fb.size() / 2u];
                 checksum += fb.back();
             }
             ankerl::nanobench::doNotOptimizeAway(checksum);
         });
}

void run_flc_core_chunks(const config& cfg,
                         const input_case& item,
                         std::vector<ankerl::nanobench::Result>& all_results) {
    if (item.codec != "flc") return;
    auto chunks = collect_flc_core_chunks(item.path);
    if (!chunks || (!chunks->error.empty() && chunks->lc.empty() && chunks->ss2.empty())) {
        return;
    }
    const auto compiled = compile_flc_core_chunks(*chunks);
    if (!compiled.error.empty()) {
        return;
    }

    {
        auto bench = make_bench(cfg, "onyx_anim FLI/FLC chunk benchmark: lc", "chunk");
        run_flc_core_chunk_case(cfg, item, *chunks, "lc", chunks->lc,
                                flc::decode_lc, bench);
        append_results(all_results, bench);
    }
    {
        auto bench = make_bench(cfg, "onyx_anim FLI/FLC compiled chunk benchmark: lc", "chunk");
        run_flc_compiled_chunk_case(cfg, item, *chunks, compiled, "lc", chunks->lc,
                                    compiled.lc, bench);
        append_results(all_results, bench);
    }
    {
        auto bench = make_bench(cfg, "onyx_anim FLI/FLC chunk benchmark: ss2", "chunk");
        run_flc_core_chunk_case(cfg, item, *chunks, "ss2", chunks->ss2,
                                flc::decode_ss2, bench);
        append_results(all_results, bench);
    }
    {
        auto bench = make_bench(cfg, "onyx_anim FLI/FLC compiled chunk benchmark: ss2", "chunk");
        run_flc_compiled_chunk_case(cfg, item, *chunks, compiled, "ss2", chunks->ss2,
                                    compiled.ss2, bench);
        append_results(all_results, bench);
    }
}

void run_codec_group(const config& cfg,
                     std::string_view codec,
                     const std::vector<std::size_t>& indices,
                     std::vector<input_case>& inputs,
                     std::vector<ankerl::nanobench::Result>& all_results) {
    {
        auto title = "onyx_anim open benchmark: " + std::string{codec};
        auto bench = make_bench(cfg, title, "open");
        for (const auto index : indices) {
            run_open_case(cfg, inputs[index], bench);
        }
        append_results(all_results, bench);
    }

    {
        auto title = "onyx_anim decode benchmark: " + std::string{codec};
        auto bench = make_bench(cfg, title, cfg.unit);
        for (const auto index : indices) {
            run_decode_case(cfg, inputs[index], bench);
        }
        append_results(all_results, bench);
    }
}

} // namespace

int main(int argc, char** argv) {
    auto cfg = parse_args(argc, argv);

#ifndef NDEBUG
    if (!cfg.allow_unoptimized) {
        fail("this benchmark was built without NDEBUG. Rebuild with optimized code "
             "(for example -DCMAKE_BUILD_TYPE=Release), or pass --allow-unoptimized "
             "only for smoke testing.");
    }
#endif

    register_codecs_once();

    std::vector<input_case> inputs;
    std::map<std::string, std::vector<std::size_t>> by_codec;

    for (const auto& path : cfg.inputs) {
        auto prepared = open_decoder(path, cfg.decode_opts);
        const auto codec = std::string{prepared.decoder->name()};
        std::string chunks;
        if (codec == "flc") {
            if (auto report = scan_flc_chunks(path)) {
                chunks = chunk_summary(*report);
            }
        }
        by_codec[codec].push_back(inputs.size());
        inputs.push_back({path, codec, std::move(prepared), std::move(chunks)});
    }

    if (!cfg.quiet) {
        print_chunk_reports(inputs);
    }

    std::vector<ankerl::nanobench::Result> all_results;
    for (const auto& [codec, indices] : by_codec) {
        run_codec_group(cfg, codec, indices, inputs, all_results);
    }
    if (cfg.chunk_core) {
        for (const auto& input : inputs) {
            run_flc_core_chunks(cfg, input, all_results);
        }
    }

    if (!cfg.json_path.empty()) {
        render_file(ankerl::nanobench::templates::json(), all_results, cfg.json_path);
    }
    if (!cfg.csv_path.empty()) {
        render_file(ankerl::nanobench::templates::csv(), all_results, cfg.csv_path);
    }
    if (!cfg.html_path.empty()) {
        render_file(ankerl::nanobench::templates::htmlBoxplot(), all_results, cfg.html_path);
    }

    return EXIT_SUCCESS;
}
