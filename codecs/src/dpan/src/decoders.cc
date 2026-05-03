#include <dpan/decoders.hh>

#include <cstring>

namespace dpan {
    namespace {
        // ----- Per-record opcode helper ---------------------------------------
        //
        // Mirrors ffmpeg's anm.c::op() — applies one of three operations to
        // `count` pixels: COPY (read from input), FILL (constant pixel) or
        // SKIP (advance past, leaving previous-frame contents). The operation
        // can span row boundaries; `x` tracks the column within the current
        // row so subsequent ops continue where this one left off.
        //
        // op_kind:
        //   copy : src points at literal pixel bytes → memcpy into dst
        //   fill : pixel >= 0  → memset dst with `pixel`
        //   skip : pixel <  0  → just advance dst, leave previous values
        //
        // Returns true if the destination buffer is exhausted (record decode
        // should stop).
        enum class op_kind { copy, fill, skip };

        struct op_state {
            std::uint8_t* dst; // current write position
            const std::uint8_t* dst_end;
            unsigned int x; // column within current row
            unsigned int width;
        };

        bool apply_op(op_state& s, op_kind k,
                      std::span <const std::uint8_t>& src,
                      int pixel, unsigned int count) noexcept {
            unsigned int remaining = s.width - s.x;
            while (count > 0u) {
                const unsigned int strip = (count < remaining) ? count : remaining;
                if (k == op_kind::copy) {
                    if (src.size() < strip) {
                        s.x = s.width - remaining;
                        return true;
                    }
                    std::memcpy(s.dst, src.data(), strip);
                    src = src.subspan(strip);
                } else if (k == op_kind::fill) {
                    std::memset(s.dst, static_cast <std::uint8_t>(pixel), strip);
                }
                // skip: nothing to write, just advance
                s.dst += strip;
                remaining -= strip;
                count -= strip;
                if (remaining == 0u) {
                    remaining = s.width;
                }
                if (s.dst >= s.dst_end) {
                    s.x = s.width - remaining;
                    return true;
                }
            }
            s.x = s.width - remaining;
            return false;
        }
    } // namespace

    // ------------------------------------------------------------------------
    // Decode one record (= one frame's worth of RunSkipDump opcodes).
    //
    // Record layout:
    //   byte 0 : 0x42 (always 'B') — record-type marker
    //   byte 1 : 0   — extra-bytes flag (non-zero unsupported in practice)
    //   bytes 2..3 : ignored (reserved / padding)
    //   bytes 4..  : opcode stream
    //
    // Opcode encoding (LE for the 16-bit cases):
    //   1) op = byte
    //      count = op & 0x7F, type = op >> 7
    //      if count != 0:
    //          type 0 → COPY `count` literal pixels from input
    //          type 1 → SKIP `count` pixels
    //   2) op == 0x00 (count=0, type=0):
    //      followed by `count` byte and `pixel` byte → FILL count copies
    //   3) op == 0x80 (count=0, type=1):
    //      followed by 16-bit LE word `(type:2 | count:14)`:
    //          count == 0:
    //              type 0 → STOP decoding
    //              type 2 → unknown opcode (ffmpeg refuses)
    //              else   → no-op, continue
    //          count != 0:
    //              type 0 → SKIP `count` pixels
    //              type 1 → SKIP `count + 0x4000` pixels
    //              type 2 → COPY `count` literal pixels
    //              type 3 → FILL `count` copies of next byte
    // ------------------------------------------------------------------------
    result decompress_record(std::span <const std::uint8_t> record,
                             std::uint8_t* dst,
                             unsigned int width,
                             unsigned int height) {
        // Empty records are valid — they mean "this frame is identical to
        // the previous one" and the framebuffer should be left untouched.
        if (record.empty()) return {};
        if (record.size() < 4u) {
            return make_unexpected("dpan: record header truncated");
        }
        if (record[0] != 0x42u) {
            return make_unexpected("dpan: record IDnum != 0x42");
        }
        if (record[1] != 0u) {
            // ffmpeg refuses non-zero padding flag; in practice nothing
            // emits it, but be loud rather than silently miscounting.
            return make_unexpected("dpan: record extra-bytes flag set");
        }
        // Skip the 4-byte record header (IDnum, flags, extrabytes u16).
        auto src = record.subspan(4);

        op_state s{dst, dst + static_cast <std::size_t>(width) * height, 0, width};

        while (!src.empty()) {
            const std::uint8_t b = src[0];
            src = src.subspan(1);
            unsigned int count = b & 0x7Fu;
            unsigned int type = b >> 7;

            if (count != 0u) {
                if (type == 0u) {
                    if (apply_op(s, op_kind::copy, src, -1, count)) break;
                } else {
                    auto dummy = std::span <const std::uint8_t>{};
                    if (apply_op(s, op_kind::skip, dummy, -1, count)) break;
                }
            } else if (type == 0u) {
                // Long FILL: count byte + pixel byte.
                if (src.size() < 2u) {
                    return make_unexpected("dpan: long FILL truncated");
                }
                const unsigned int n = src[0];
                const std::uint8_t pixel = src[1];
                src = src.subspan(2);
                if (n == 0u) continue; // 0x00 0x00 0x?? = no-op
                auto dummy = std::span <const std::uint8_t>{};
                if (apply_op(s, op_kind::fill, dummy, pixel, n)) break;
            } else {
                // 16-bit LE escape.
                if (src.size() < 2u) {
                    return make_unexpected("dpan: 16-bit opcode truncated");
                }
                const unsigned int word =
                    static_cast <unsigned int>(src[0]) |
                    (static_cast <unsigned int>(src[1]) << 8);
                src = src.subspan(2);
                const unsigned int wcount = word & 0x3FFFu;
                const unsigned int wtype = word >> 14;
                if (wcount == 0u) {
                    if (wtype == 0u) break; // STOP
                    if (wtype == 2u) {
                        return make_unexpected("dpan: unsupported 16-bit opcode");
                    }
                    continue; // wtype 1 or 3 with count 0 = no-op
                }
                if (wtype == 2u) {
                    if (apply_op(s, op_kind::copy, src, -1, wcount)) break;
                } else if (wtype == 3u) {
                    if (src.empty()) {
                        return make_unexpected("dpan: 16-bit FILL pixel truncated");
                    }
                    const std::uint8_t pixel = src[0];
                    src = src.subspan(1);
                    auto dummy = std::span <const std::uint8_t>{};
                    if (apply_op(s, op_kind::fill, dummy, pixel, wcount)) break;
                } else {
                    // wtype 0 or 1 → SKIP (with type 1 adding 0x4000).
                    auto dummy = std::span <const std::uint8_t>{};
                    const unsigned int skip_count =
                        wtype == 1u ? (wcount + 0x4000u) : wcount;
                    if (apply_op(s, op_kind::skip, dummy, -1, skip_count)) break;
                }
            }
        }
        return {};
    }
} // namespace dpan
