#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <dpan/header.hh>
#include <dpan/decoders.hh>

#include <musac/sdk/io_stream.hh>

#include "dpan.hh"
#include "codec_common.hh"

namespace onyx_anim {
    namespace {
        constexpr std::array<std::string_view, 4> kDpanExtensions = {
            ".anm", ".ANM", ".gam", ".GAM"
        };

        using detail::read_full_file;

        // ----- Decoder --------------------------------------------------------

        class dpan_decoder_impl final : public anim_decoder {
            public:
                using anim_decoder::open;

                [[nodiscard]] std::string_view name() const noexcept override {
                    return dpan_decoder::codec_name;
                }
                [[nodiscard]] std::span<const std::string_view>
                extensions() const noexcept override {
                    return {kDpanExtensions.data(), kDpanExtensions.size()};
                }

                [[nodiscard]] bool sniff(musac::io_stream* s) const override {
                    if (!s) return false;
                    const auto pos = s->tell();
                    std::uint8_t buf[24] = {};
                    const auto n = s->read(buf, sizeof(buf));
                    s->seek(pos, musac::seek_origin::set);
                    if (n < sizeof(buf)) return false;
                    // Magic at 0..3 = "LPF ", magic at 16..19 = "ANIM",
                    // and width/height (LE u16 at 20/22) must be non-zero.
                    return buf[0]=='L' && buf[1]=='P' && buf[2]=='F' && buf[3]==' ' &&
                           buf[16]=='A' && buf[17]=='N' && buf[18]=='I' && buf[19]=='M' &&
                           (buf[20] | buf[21]) != 0u &&
                           (buf[22] | buf[23]) != 0u;
                }

                [[nodiscard]] result open(musac::io_stream* s,
                                          const decode_options& opts) override {
                    if (!s) return make_unexpected<error_type>("dpan: null stream");
                    if (!read_full_file(s, file_bytes_)) {
                        return make_unexpected<error_type>("dpan: cannot read file");
                    }
                    if (file_bytes_.size() < dpan::kHeaderSize) {
                        return make_unexpected<error_type>("dpan: file smaller than header");
                    }

                    auto h = dpan::parse_file_header(
                        {file_bytes_.data(), file_bytes_.size()});
                    if (!h) return make_unexpected<error_type>(h.error());
                    header_ = *h;

                    if (header_.width  > opts.max_width ||
                        header_.height > opts.max_height) {
                        return make_unexpected<error_type>(
                            "dpan: dimensions exceed configured limit");
                    }

                    // ----- Palette: 256 × u32 LE at file offset 256, right
                    // after the 256-byte file header. Bytes on disk are
                    // stored as (B, G, R, _) — standard DOS BGR ordering.
                    // ffmpeg reads each entry as a LE u32 and then extracts
                    // R/G/B at bit positions 16/8/0 of that integer, which
                    // is byte-equivalent to taking byte 2 for R, byte 1 for
                    // G, byte 0 for B.
                    constexpr std::size_t kPaletteOffset = 256;
                    constexpr std::size_t kPaletteBytes  = 4 * 256;
                    palette_888_.assign(256u * 3u, 0);
                    if (file_bytes_.size() >= kPaletteOffset + kPaletteBytes) {
                        const std::uint8_t* p = file_bytes_.data() + kPaletteOffset;
                        for (std::size_t i = 0; i < 256; ++i) {
                            palette_888_[i * 3 + 0] = p[i * 4 + 2]; // R (= disk byte 2)
                            palette_888_[i * 3 + 1] = p[i * 4 + 1]; // G
                            palette_888_[i * 3 + 2] = p[i * 4 + 0]; // B (= disk byte 0)
                        }
                    }

                    // ----- Page table at offset lpf_table_offset (1280):
                    // 256 × 6-byte entries.
                    pages_.assign(dpan::kPageTableEntries, dpan::page_entry{});
                    const std::size_t pt_off = header_.lpf_table_offset;
                    if (file_bytes_.size() <
                        pt_off + dpan::kPageTableEntries * dpan::kPageEntrySize) {
                        return make_unexpected<error_type>(
                            "dpan: page table truncated");
                    }
                    for (std::size_t i = 0; i < dpan::kPageTableEntries; ++i) {
                        auto e = dpan::parse_page_entry(
                            {file_bytes_.data() + pt_off + i * dpan::kPageEntrySize,
                             dpan::kPageEntrySize});
                        if (!e) return make_unexpected<error_type>(e.error());
                        pages_[i] = *e;
                    }

                    // ----- Build a flat per-record (frame_offset, frame_size)
                    // index in LOGICAL record order. The page table is
                    // sorted by PHYSICAL layout, not by frame index — pages
                    // can appear out of record order on disk (page 0 might
                    // hold frames 0-11, page 1 frames 23-124, page 2 frames
                    // 12-22, etc.). For each page we know its base_record;
                    // we collect (logical_frame, file_offset, size) tuples
                    // and sort them at the end so decode_frame can walk in
                    // ascending frame order.
                    const unsigned int total_records = header_.has_last_delta
                        ? std::max<unsigned int>(header_.n_records, 1u) - 1u
                        : header_.n_records;

                    struct record_entry {
                        unsigned int frame;
                        std::size_t  offset;
                        std::uint16_t size;
                    };
                    std::vector<record_entry> entries;
                    entries.reserve(total_records);

                    constexpr std::size_t kPageStride = 1u << 16; // 64 KiB
                    for (std::size_t pi = 0; pi < dpan::kPageTableEntries; ++pi) {
                        const auto& pe = pages_[pi];
                        const unsigned int nrecs = pe.n_records & 0x3FFFu;
                        if (nrecs == 0u) continue;
                        const std::size_t page_off = dpan::kHeaderSize +
                                                     pi * kPageStride;
                        if (page_off >= file_bytes_.size()) break;

                        // Page header: u16 base / u16 nrecs / u16 nbytes /
                        // u16 bytes_continued — 8 bytes total. Then
                        // u16 record_sizes[nrecs].
                        const std::size_t sizes_off = page_off + 8u;
                        const std::size_t data_off  = sizes_off + nrecs * 2u;
                        if (data_off > file_bytes_.size()) {
                            return make_unexpected<error_type>(
                                "dpan: page sizes table overruns file");
                        }
                        std::size_t cur = data_off;
                        for (unsigned int r = 0; r < nrecs; ++r) {
                            const std::size_t so = sizes_off + r * 2u;
                            const std::uint16_t rsz = static_cast<std::uint16_t>(
                                file_bytes_[so] |
                                (file_bytes_[so + 1] << 8));
                            if (cur + rsz > file_bytes_.size()) {
                                return make_unexpected<error_type>(
                                    "dpan: record overruns file");
                            }
                            const unsigned int frame = pe.base_record + r;
                            if (frame < total_records) {
                                entries.push_back({frame, cur, rsz});
                            }
                            cur += rsz;
                        }
                    }

                    if (entries.empty()) {
                        return make_unexpected<error_type>("dpan: no records found");
                    }
                    std::sort(entries.begin(), entries.end(),
                              [](const record_entry& a, const record_entry& b) {
                                  return a.frame < b.frame;
                              });

                    record_offsets_.assign(entries.size(), 0u);
                    record_sizes_.assign(entries.size(), 0u);
                    for (std::size_t i = 0; i < entries.size(); ++i) {
                        // Verify dense frame coverage: any gap means the
                        // file's page descriptors are inconsistent.
                        if (entries[i].frame != i) {
                            return make_unexpected<error_type>(
                                "dpan: page descriptors leave gaps in record order");
                        }
                        record_offsets_[i] = entries[i].offset;
                        record_sizes_[i]   = entries[i].size;
                    }

                    // Working buffer — RunSkipDump applies on top of the
                    // previous frame, so we keep one persistent chunky
                    // buffer that decode_frame() patches in place.
                    fb_chunky_.assign(
                        static_cast<std::size_t>(header_.width) * header_.height, 0);

                    // anim_info.
                    info_.width        = header_.width;
                    info_.height       = header_.height;
                    info_.frame_count  = static_cast<unsigned int>(record_offsets_.size());
                    info_.format       = pixel_format::indexed8;
                    const unsigned int fps =
                        header_.fps > 0u ? header_.fps : 18u; // safe default
                    info_.frame_period = std::chrono::microseconds(
                        1'000'000LL / static_cast<std::int64_t>(fps));
                    info_.duration = info_.frame_period * info_.frame_count;

                    cursor_ = 0;
                    return {};
                }

                [[nodiscard]] const anim_info& info() const noexcept override {
                    return info_;
                }

                [[nodiscard]] frame_result decode_frame(onyx_image::surface& out) override {
                    if (cursor_ >= record_offsets_.size()) {
                        return make_unexpected<error_type>("dpan: end of stream");
                    }
                    const auto idx = cursor_;
                    const std::size_t off = record_offsets_[idx];
                    const std::size_t sz  = record_sizes_[idx];
                    if (off + sz > file_bytes_.size()) {
                        return make_unexpected<error_type>(
                            "dpan: record range past end of file");
                    }
                    auto record = std::span<const std::uint8_t>(
                        file_bytes_.data() + off, sz);
                    if (auto r = dpan::decompress_record(
                            record, fb_chunky_.data(),
                            header_.width, header_.height); !r) {
                        return make_unexpected<error_type>(r.error());
                    }

                    if (!out.set_size(static_cast<int>(header_.width),
                                      static_cast<int>(header_.height),
                                      pixel_format::indexed8)) {
                        return make_unexpected<error_type>(
                            "dpan: surface set_size failed");
                    }
                    out.set_palette_size(256);
                    out.write_palette(0, std::span<const std::uint8_t>{
                        palette_888_.data(), palette_888_.size()});
                    for (unsigned int y = 0; y < header_.height; ++y) {
                        out.write_pixels(0, static_cast<int>(y),
                            static_cast<int>(header_.width),
                            fb_chunky_.data() +
                                static_cast<std::size_t>(y) * header_.width);
                    }

                    frame_info fi{};
                    fi.index    = static_cast<unsigned int>(idx);
                    fi.pts      = info_.frame_period * static_cast<std::int64_t>(idx);
                    fi.duration = info_.frame_period;
                    fi.keyframe = (idx == 0u);
                    ++cursor_;
                    return fi;
                }

                [[nodiscard]] bool eof() const noexcept override {
                    return cursor_ >= record_offsets_.size();
                }

                bool rewind() override { return seek_to_frame(0u); }

                bool seek_to_frame(unsigned int idx) override {
                    if (record_offsets_.empty()) return false;
                    if (idx > record_offsets_.size()) return false;
                    if (idx == cursor_) return true;
                    // RunSkipDump records are deltas on top of the prior
                    // frame, so jumping forward needs replay from frame 0.
                    if (idx < cursor_) {
                        cursor_ = 0;
                        std::fill(fb_chunky_.begin(), fb_chunky_.end(),
                                  std::uint8_t{0});
                    }
                    onyx_image::memory_surface tmp;
                    while (cursor_ < idx) {
                        if (!decode_frame(tmp)) return false;
                    }
                    return true;
                }

                bool seek_to_time(std::chrono::microseconds pts) override {
                    if (info_.frame_period.count() <= 0) return false;
                    if (record_offsets_.empty()) return false;
                    std::int64_t f = pts.count() < 0
                        ? 0
                        : pts.count() / info_.frame_period.count();
                    const auto last =
                        static_cast<std::int64_t>(record_offsets_.size());
                    if (f > last) f = last;
                    return seek_to_frame(static_cast<unsigned int>(f));
                }

            private:
                std::vector<std::uint8_t>     file_bytes_;
                dpan::file_header             header_{};
                std::vector<dpan::page_entry> pages_;
                std::vector<std::size_t>      record_offsets_;
                std::vector<std::uint16_t>    record_sizes_;
                std::vector<std::uint8_t>     palette_888_;
                std::vector<std::uint8_t>     fb_chunky_;

                anim_info                     info_{};
                std::size_t                   cursor_ = 0;
        };
    } // namespace

    std::unique_ptr<anim_decoder> dpan_decoder::create() {
        return std::make_unique<dpan_decoder_impl>();
    }
} // namespace onyx_anim
