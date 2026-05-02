#include <flc/header.hh>

namespace flc {
    // Layout per the Animator Pro / FLIC spec (FLIC_HEADER, 128 bytes total):
    //   bytes 0..21   shared FLI/FLC fields (size, magic, frame_count, w, h,
    //                 depth, flags, speed_units, reserved1)
    //   bytes 22..47  FLC-only fields up through totalframes
    //                   (created/creator/updated/updater [4*4 B],
    //                    aspect_dx/dy [2*2 B], ext_flags/keyframes/totalframes
    //                    [3*2 B])
    //   bytes 48..51  req_memory (DWORD per spec)
    //   bytes 52..55  max_regions, transp_num
    //   bytes 56..79  reserved2 (24 bytes, must be zero)
    //   bytes 80..87  oframe1, oframe2
    //   bytes 88..127 reserved3 (40 bytes, must be zero)
    //
    // FLI files leave the extended fields undefined; we skip reading them and
    // rely on value-initialisation to leave the struct members zero.
    inline constexpr std::size_t kFlcExtendedEnd = 88;
    inline constexpr std::size_t kReserved2Bytes = 24;

    expected<file_header>
    parse_file_header(std::span<const std::uint8_t> data) {
        if (data.size() < kMinHeaderBytes) {
            return make_unexpected("flc: file header truncated");
        }
        byte_reader br{data};
        file_header h{};

        // ---- shared fields (bytes 0..21) ---------------------------------
        br >> h.size >> h.magic;
        if (h.magic != kMagicFli && h.magic != kMagicFlc) {
            return make_unexpected("flc: invalid file magic");
        }
        br >> h.frame_count
           >> h.width >> h.height
           >> h.depth >> h.flags
           >> h.speed_units
           >> h.reserved1;

        // ---- FLC-only extended fields (bytes 22..85) ---------------------
        if (h.is_flc()) {
            if (data.size() < kFlcExtendedEnd) {
                return make_unexpected("flc: FLC extended header truncated");
            }
            br >> h.created  >> h.creator
               >> h.updated  >> h.updater
               >> h.aspect_dx >> h.aspect_dy
               >> h.ext_flags >> h.keyframes >> h.totalframes
               >> h.req_memory >> h.max_regions >> h.transp_num
               >> skip(kReserved2Bytes)        // reserved2: must be zero
               >> h.oframe1 >> h.oframe2;
            // reserved3 (bytes 86..127) is intentionally not consumed; the
            // caller may have given us only the first kFlcExtendedEnd bytes.
        }

        if (!br) {
            return make_unexpected("flc: file header truncated mid-read");
        }
        return h;
    }

    expected<frame_header>
    parse_frame_header(std::span<const std::uint8_t> data) {
        if (data.size() < kFrameHeaderSize) {
            return make_unexpected("flc: frame header truncated");
        }
        byte_reader br{data};
        frame_header h{};
        br >> h.size >> h.magic;
        if (h.magic != kFrameMagicStandard &&
            h.magic != kFrameMagicPrefix &&
            h.magic != kFrameMagicCelData &&
            h.magic != kFrameMagicVariant) {
            return make_unexpected("flc: invalid frame magic");
        }
        br >> h.sub_chunks
           >> h.delay
           >> h.reserved
           >> h.width
           >> h.height;
        if (!br) {
            return make_unexpected("flc: frame header truncated mid-read");
        }
        return h;
    }
} // namespace flc
