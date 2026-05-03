#include <dpan/header.hh>

namespace dpan {
    expected<file_header>
    parse_file_header(std::span<const std::uint8_t> data) {
        if (data.size() < kHeaderSize) {
            return make_unexpected("dpan: file header too small");
        }
        // Magic bytes are stored in disk order — read as raw u32 in BE so
        // we can compare against ASCII fourcc constants.
        const auto* p = data.data();
        const std::uint32_t magic =
            (static_cast<std::uint32_t>(p[0]) << 24) |
            (static_cast<std::uint32_t>(p[1]) << 16) |
            (static_cast<std::uint32_t>(p[2]) <<  8) |
             static_cast<std::uint32_t>(p[3]);
        if (magic != kLpfMagic) {
            return make_unexpected("dpan: missing 'LPF ' magic");
        }

        // Body fields are little-endian.
        byte_reader br{data.subspan(4)};
        file_header h{};
        br >> h.max_lps >> h.n_lps >> h.n_records
           >> h.max_recs_per_lp >> h.lpf_table_offset;
        if (!br) return make_unexpected("dpan: file header truncated mid-read");

        // Verify "ANIM" content tag at offset 16.
        const std::uint32_t content =
            (static_cast<std::uint32_t>(p[16]) << 24) |
            (static_cast<std::uint32_t>(p[17]) << 16) |
            (static_cast<std::uint32_t>(p[18]) <<  8) |
             static_cast<std::uint32_t>(p[19]);
        if (content != kAnimMagic) {
            return make_unexpected("dpan: missing 'ANIM' content tag");
        }

        byte_reader br2{data.subspan(20)};
        br2 >> h.width >> h.height
            >> h.variant >> h.version
            >> h.has_last_delta >> h.last_delta_valid
            >> h.pixel_type
            >> h.compression_type
            >> h.other_recs_per_frm
            >> h.bitmap_type;
        if (!br2) return make_unexpected("dpan: file header dim/flags truncated");

        // 32 bytes of recordTypes[] follow at offset 32.
        if (data.size() < 64u + 4u) {
            return make_unexpected("dpan: file header tail truncated");
        }
        byte_reader br3{data.subspan(64)};
        br3 >> h.n_frames >> h.fps;

        // Hardcoded constants per the spec.
        if (h.max_lps != 256u) {
            return make_unexpected("dpan: unsupported max_lps != 256");
        }
        if (h.lpf_table_offset != 1280u) {
            return make_unexpected("dpan: unsupported lpf_table_offset != 1280");
        }
        if (h.variant != 0u) {
            return make_unexpected("dpan: variant != 0 (only ANIM supported)");
        }
        if (h.pixel_type != 0u) {
            return make_unexpected("dpan: pixel_type != 0 (only 256-colour supported)");
        }
        // compression_type 1 = RunSkipDump (the only documented value).
        // Some real-world files (HORSE.ANM in our corpus) advertise 0 but
        // their records still start with the RunSkipDump 0x42 IDnum and
        // decode correctly — the header byte is just a non-standard
        // authoring choice. ffmpeg rejects 0 outright; we accept it and
        // let the per-record IDnum check in decompress_record catch any
        // genuinely-different encoding.
        if (h.compression_type != 0u && h.compression_type != 1u) {
            return make_unexpected("dpan: only RunSkipDump compression supported");
        }
        if (h.bitmap_type != 1u) {
            return make_unexpected("dpan: unsupported bitmap_type != 1");
        }
        if (h.width == 0u || h.height == 0u) {
            return make_unexpected("dpan: zero dimensions");
        }
        return h;
    }

    expected<page_entry>
    parse_page_entry(std::span<const std::uint8_t> data) {
        if (data.size() < kPageEntrySize) {
            return make_unexpected("dpan: page entry truncated");
        }
        byte_reader br{data};
        page_entry e{};
        br >> e.base_record >> e.n_records >> e.size;
        if (!br) return make_unexpected("dpan: page entry truncated mid-read");
        return e;
    }
} // namespace dpan
