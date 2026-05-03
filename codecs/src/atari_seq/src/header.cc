#include <atari_seq/header.hh>

namespace atari_seq {
    expected <file_header>
    parse_file_header(std::span <const std::uint8_t> data) {
        if (data.size() < kFileHeaderSize) {
            return make_unexpected("seq: file header truncated");
        }
        byte_reader br{data};
        file_header h{};
        br >> h.magic;
        if (h.magic != kMagicCyber && h.magic != kMagicFlicker) {
            return make_unexpected("seq: invalid magic");
        }
        br >> h.version;
        if (h.version != 0) {
            return make_unexpected("seq: unsupported version");
        }
        br >> h.frame_count >> h.speed;
        // bytes 12..127 are reserved; we don't read them.
        if (!br) {
            return make_unexpected("seq: file header truncated mid-read");
        }
        return h;
    }

    expected <cel_header>
    parse_cel_header(std::span <const std::uint8_t> data) {
        if (data.size() < kCelHeaderSize) {
            return make_unexpected("seq: cel header truncated");
        }
        byte_reader br{data};
        cel_header h{};

        br >> h.type;
        if (h.type != 0xFFFFu) {
            return make_unexpected("seq: invalid cel type (expected 0xFFFF)");
        }
        br >> h.resolution;
        if (h.resolution > 2) {
            return make_unexpected("seq: invalid resolution");
        }

        for (unsigned int i = 0; i < 16; ++i) {
            br >> h.palette[i];
        }

        // 12 bytes filename, then 6 bytes color animation block (flag + range +
        // active + speeddir + steps[u16]) — both skipped.
        br >> skip(12) >> skip(6);

        br >> h.x_offset >> h.y_offset >> h.width >> h.height;

        std::uint8_t op_byte = 0;
        std::uint8_t sm_byte = 0;
        br >> op_byte >> sm_byte;
        if (op_byte > 1) {
            return make_unexpected("seq: invalid operation");
        }
        if (sm_byte > 1) {
            return make_unexpected("seq: invalid storage method");
        }
        h.op = static_cast <operation>(op_byte);
        h.sm = static_cast <storage>(sm_byte);

        br >> h.data_size;
        // 60 bytes reserved tail — not read; caller is expected to seek past
        // the full kCelHeaderSize bytes if it cares about positioning.

        if (!br) {
            return make_unexpected("seq: cel header truncated mid-read");
        }
        return h;
    }

    expected <resolution_info>
    info_for_resolution(std::uint16_t resolution) {
        switch (resolution) {
            case 0: return resolution_info{320, 200, 4, 16};
            case 1: return resolution_info{640, 200, 2, 4};
            case 2: return resolution_info{640, 400, 1, 2};
            default: return make_unexpected("seq: invalid resolution");
        }
    }
} // namespace atari_seq
