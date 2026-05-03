#pragma once

#include <dpan/types.hh>

#include <cstdint>
#include <span>

namespace dpan {
    // Spec: docs/deluxe_paint.txt; demuxer reference: ffmpeg
    // libavformat/anm.c. All multi-byte fields are little-endian (DOS).
    //
    // File layout (in order):
    //   0..2815   File header (this struct + 256-entry palette + page table
    //             header copies — we only consume the first ~150 bytes).
    //   1280..2815  256 × 6-byte Page descriptors.
    //   2816..    Pages: each is at offset header_end + (page_index << 16),
    //             64 KiB except the last. Each page begins with its own
    //             header (see `page_header`) followed by record_sizes[N]
    //             u16s and then the record bytes.

    inline constexpr std::uint32_t kLpfMagic  = 0x4C504620u; // "LPF " (BE order)
    inline constexpr std::uint32_t kAnimMagic = 0x414E494Du; // "ANIM"
    inline constexpr std::size_t   kHeaderSize    = 2816;
    inline constexpr std::size_t   kPageTableEntries = 256; // hard-coded
    inline constexpr std::size_t   kPageEntrySize    = 6;

    struct file_header {
        std::uint16_t max_lps;          // always 256
        std::uint16_t n_lps;            // # of large pages used
        std::uint32_t n_records;        // total # of records (frames)
        std::uint16_t max_recs_per_lp;  // always 256
        std::uint16_t lpf_table_offset; // always 1280
        std::uint16_t width;
        std::uint16_t height;
        std::uint8_t  variant;          // 0 = ANIM
        std::uint8_t  version;          // frame-rate clock select
        std::uint8_t  has_last_delta;   // last record is loop-back delta
        std::uint8_t  last_delta_valid;
        std::uint8_t  pixel_type;       // 0 = 256-colour
        std::uint8_t  compression_type; // 1 = RunSkipDump
        std::uint8_t  other_recs_per_frm;
        std::uint8_t  bitmap_type;      // 1 = 320×200×256
        std::uint32_t n_frames;         // includes loop-back delta if present
        std::uint16_t fps;
    };

    // 6-byte page descriptor at offset 1280.
    struct page_entry {
        std::uint16_t base_record;
        std::uint16_t n_records;        // bit 15 = continued from prev,
                                        // bit 14 = continues into next
        std::uint16_t size;             // payload bytes in this page
                                        // (excluding the 8-byte header)
    };

    [[nodiscard]] expected<file_header>
        parse_file_header(std::span<const std::uint8_t> data);

    [[nodiscard]] expected<page_entry>
        parse_page_entry(std::span<const std::uint8_t> data);
} // namespace dpan
