#include <yafa/decoders.hh>

#include <algorithm>
#include <cstring>

namespace yafa {
    namespace {
        constexpr std::uint32_t kXPKF = 0x58504B46u; // "XPKF"

        constexpr std::uint32_t fourcc(const char (&s)[5]) {
            return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) <<  8) |
                    static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3]));
        }
    } // namespace

    // ------------------------------------------------------------------------
    // XPK file walker. Header layout (36 bytes minimum):
    //
    //     0..3   "XPKF"
    //     4..7   packed_size (BE) — total chunk = 8 + packed_size
    //     8..11  type FourCC ("FAST", "NUKE", ...)
    //     12..15 raw_size (BE) — total uncompressed bytes
    //     16..31 first 16 bytes of raw (verification)
    //     32     flags: bit 0 = long_headers, bit 1 = password,
    //                   bit 2 = extra_header
    //     33     reserved
    //     34..35 header checksum
    //     [36..] optional extra header (if flags & 4): u16 extra_len, then
    //            `extra_len` bytes
    //     [then chunks]
    //
    // Inner chunks: 8-byte header (12 if long_headers).
    //     0     chunk type (0=raw, 1=compressed, 15=last)
    //     1     header checksum (xor of header bytes)
    //     2..3  chunk-data checksum
    //     4..5  packed_size  (long_headers: 4..7)
    //     6..7  raw_size     (long_headers: 8..11)
    //     [data: packed_size bytes, padded to 4-byte alignment]
    //
    // We accept long_headers and ignore checksums. Mirrors temisu/ancient's
    // XPKMain.cpp, simplified for the sub-libraries YAFA actually uses.
    // ------------------------------------------------------------------------
    expected<std::vector<std::uint8_t>>
    xpk_decompress(std::span<const std::uint8_t> packed) {
        if (packed.size() < 36u) {
            return make_unexpected("yafa: XPK header too small");
        }
        byte_reader br{packed};
        std::uint32_t magic = 0, packed_size = 0, type = 0, raw_size = 0;
        br >> magic >> packed_size >> type >> raw_size;
        if (!br || magic != kXPKF) {
            return make_unexpected("yafa: not an XPKF stream");
        }
        if (raw_size == 0u || packed_size == 0u) {
            return make_unexpected("yafa: XPK zero raw/packed size");
        }
        if (8u + static_cast<std::size_t>(packed_size) > packed.size()) {
            return make_unexpected("yafa: XPK packed_size overruns buffer");
        }

        // Skip first-16-bytes-of-raw + reserved nibble before flags.
        const std::uint8_t flags = packed[32];
        const bool long_headers = (flags & 0x01u) != 0u;
        const bool has_password = (flags & 0x02u) != 0u;
        const bool has_extra    = (flags & 0x04u) != 0u;
        if (has_password) {
            return make_unexpected("yafa: password-protected XPK not supported");
        }

        std::size_t header_size = 36u;
        if (has_extra) {
            if (packed.size() < 38u) {
                return make_unexpected("yafa: XPK extra header truncated");
            }
            const std::uint16_t extra_len = static_cast<std::uint16_t>(
                (static_cast<unsigned>(packed[36]) << 8) | packed[37]);
            header_size = 38u + extra_len;
            if (header_size > 8u + packed_size) {
                return make_unexpected("yafa: XPK extra header out of range");
            }
        }

        std::vector<std::uint8_t> out;
        out.resize(raw_size);

        const std::size_t chunk_hdr_len = long_headers ? 12u : 8u;
        std::size_t offset = header_size;
        std::size_t out_offset = 0;
        bool saw_last = false;

        while (offset < 8u + packed_size && !saw_last) {
            if (offset + chunk_hdr_len > packed.size()) {
                return make_unexpected("yafa: XPK chunk header truncated");
            }
            const std::uint8_t ctype = packed[offset];
            std::uint32_t cpacked = 0, craw = 0;
            if (long_headers) {
                cpacked =
                    (static_cast<std::uint32_t>(packed[offset + 4]) << 24) |
                    (static_cast<std::uint32_t>(packed[offset + 5]) << 16) |
                    (static_cast<std::uint32_t>(packed[offset + 6]) <<  8) |
                     static_cast<std::uint32_t>(packed[offset + 7]);
                craw =
                    (static_cast<std::uint32_t>(packed[offset + 8])  << 24) |
                    (static_cast<std::uint32_t>(packed[offset + 9])  << 16) |
                    (static_cast<std::uint32_t>(packed[offset + 10]) <<  8) |
                     static_cast<std::uint32_t>(packed[offset + 11]);
            } else {
                cpacked =
                    (static_cast<std::uint32_t>(packed[offset + 4]) << 8) |
                     static_cast<std::uint32_t>(packed[offset + 5]);
                craw =
                    (static_cast<std::uint32_t>(packed[offset + 6]) << 8) |
                     static_cast<std::uint32_t>(packed[offset + 7]);
            }
            const std::size_t data_off = offset + chunk_hdr_len;
            if (data_off + cpacked > packed.size()) {
                return make_unexpected("yafa: XPK chunk data truncated");
            }
            if (out_offset + craw > out.size()) {
                return make_unexpected("yafa: XPK chunk overruns raw buffer");
            }

            const auto chunk_packed =
                packed.subspan(data_off, cpacked);
            const auto chunk_raw =
                std::span<std::uint8_t>{out.data() + out_offset, craw};

            if (ctype == 0u) {
                // Raw (uncompressed) chunk — copy verbatim.
                if (cpacked != craw) {
                    return make_unexpected(
                        "yafa: XPK raw chunk packed != raw size");
                }
                std::memcpy(chunk_raw.data(), chunk_packed.data(), craw);
            } else if (ctype == 1u || ctype == 15u) {
                // Compressed via the file-level sub-library. Type 15 is also
                // a compressed chunk — it just signals the end of the chunk
                // list.
                switch (type) {
                    case 0x46415354u: { // "FAST"
                        if (auto r = xpk_fast_decompress(chunk_packed, chunk_raw); !r) {
                            return make_unexpected(r.error());
                        }
                        break;
                    }
                    case 0x4E554B45u: { // "NUKE"
                        if (auto r = xpk_nuke_decompress(chunk_packed, chunk_raw); !r) {
                            return make_unexpected(r.error());
                        }
                        break;
                    }
                    default:
                        return make_unexpected(
                            "yafa: unsupported XPK sub-library");
                }
            } else {
                return make_unexpected("yafa: unknown XPK chunk type");
            }

            out_offset += craw;
            // Chunks are padded to 4-byte alignment.
            offset = data_off + ((cpacked + 3u) & ~std::size_t{3u});
            if (ctype == 15u) saw_last = true;
        }
        if (!saw_last) {
            return make_unexpected("yafa: XPK chunk list missing terminator");
        }
        if (out_offset != raw_size) {
            return make_unexpected("yafa: XPK output size mismatch");
        }
        return out;
    }

    // ------------------------------------------------------------------------
    // XPK FAST — Christian von Roques' "fast LZ77".
    //
    // Two streams over the same packed buffer:
    //   - forward stream reads literal bytes (one at a time)
    //   - backward stream (from end of buffer, MSB-first within each byte)
    //     reads control bits and 16-bit copy descriptors
    //
    // For each control bit:
    //   0 → write one literal byte from the forward stream
    //   1 → read u16 BE from the backward stream:
    //         low  4 bits → run-length tag; copy count = 18 - tag
    //         high 12 bits → distance (back into already-emitted output)
    //
    // Decoding stops when the output buffer is full. Mirrors
    // temisu/ancient's FASTDecompressor.cpp; the bit-stream geometry is what
    // makes this a 50-line algorithm.
    // ------------------------------------------------------------------------
    namespace {
        // Little forward / backward stream pair sharing an underlying buffer.
        // Forward reads bytes from the start; backward reads bytes (and bits,
        // MSB-first within each byte) from the end. They walk towards each
        // other; reading is bounded by the per-stream "limit" which the other
        // stream advances as it consumes bytes.
        struct fwd_bwd_streams {
            const std::uint8_t* data;
            std::size_t fwd_pos;
            std::size_t bwd_pos;       // points one past the next byte to read
            std::uint16_t bit_buf = 0; // MSB-first 16-bit shift register
            unsigned int bit_count = 0;

            std::uint8_t read_fwd() noexcept {
                return data[fwd_pos++];
            }
            std::uint16_t read_be16_bwd() noexcept {
                bwd_pos -= 2;
                return static_cast<std::uint16_t>(
                    (static_cast<unsigned>(data[bwd_pos]) << 8) |
                     data[bwd_pos + 1]);
            }
            std::uint32_t read_bit_bwd() noexcept {
                if (bit_count == 0u) {
                    bit_buf = read_be16_bwd();
                    bit_count = 16u;
                }
                const std::uint32_t b = (bit_buf >> 15) & 1u;
                bit_buf = static_cast<std::uint16_t>(bit_buf << 1);
                --bit_count;
                return b;
            }
        };
    } // namespace

    result xpk_fast_decompress(
        std::span<const std::uint8_t> packed,
        std::span<std::uint8_t>       raw) {
        if (raw.empty()) return {}; // terminator / pad chunk, nothing to do
        if (packed.size() < 2u) {
            return make_unexpected("yafa: FAST chunk too small");
        }
        fwd_bwd_streams s{packed.data(), 0, packed.size()};
        std::size_t out_pos = 0;
        const std::size_t out_size = raw.size();

        while (out_pos < out_size) {
            // Cheap exhaustion check; any read past stream bounds would be
            // an underrun on the packed data.
            if (s.fwd_pos >= s.bwd_pos) {
                return make_unexpected("yafa: FAST stream underrun");
            }
            const std::uint32_t ctrl = s.read_bit_bwd();
            if (ctrl == 0u) {
                if (s.fwd_pos >= s.bwd_pos) {
                    return make_unexpected("yafa: FAST literal underrun");
                }
                raw[out_pos++] = s.read_fwd();
            } else {
                if (s.bwd_pos < 2u || s.bwd_pos < 2u + s.fwd_pos) {
                    return make_unexpected("yafa: FAST descriptor underrun");
                }
                const std::uint16_t ld = s.read_be16_bwd();
                const std::uint32_t tag      = ld & 0x000Fu;
                const std::uint32_t distance = static_cast<std::uint32_t>(ld >> 4);
                const std::size_t avail = out_size - out_pos;
                std::size_t count =
                    std::min<std::size_t>(static_cast<std::size_t>(18u - tag), avail);
                if (distance == 0u || distance > out_pos) {
                    return make_unexpected("yafa: FAST distance out of range");
                }
                // Byte-by-byte copy supports overlapping (LZ77 repeat).
                for (std::size_t i = 0; i < count; ++i) {
                    raw[out_pos] = raw[out_pos - distance];
                    ++out_pos;
                }
            }
        }
        return {};
    }

    // ------------------------------------------------------------------------
    // XPK NUKE — LZ77 with multi-stream control bits and a variable-length
    // distance code. Ported from temisu/ancient's NUKEDecompressor.cpp.
    //
    // Streams (all sharing the same packed buffer):
    //   - backward byte stream: literal bytes (read from the end backwards)
    //   - forward bit stream:   four logical bit-streams interleaved into
    //                           the forward bytes, MSB-first within each
    //                           byte; we model that as four independent
    //                           bit-readers all pulling from the same
    //                           forward sequence.
    //
    // Per token:
    //   bit1 == 0 → literal run:
    //       if bit1 == 1 → 1 byte
    //       else: count = 0; loop: read 2 bits;
    //             if 0: count += 3, continue; else: count += 5 - 2bits, stop.
    //       Then read `count` bytes from the backward stream into output.
    //   Then if !eof:
    //     read 4-bit distance index (LSB-first from the int32 reader);
    //     decode VLC distance from a 16-entry table with bit lengths
    //         {4,6,8,9,-4,7,9,11,13,14,-5,7,9,11,13,14}
    //         (a leading minus marks the start of the third sub-table; the
    //         absolute value is the bit length for that entry's index range).
    //     count: if index < 4 → 2; index < 10 → 3; else read more bits.
    //   Copy `count` bytes from output[pos-distance].
    //
    // Implemented as a self-contained walker with four independent forward
    // bit-stream cursors, mirroring ancient's MSBBitReader pattern.
    // ------------------------------------------------------------------------
    namespace {
        // NUKE uses three flavours of bit reader, all four sharing the same
        // forward byte cursor but maintaining independent bit accumulators:
        //
        //   - bit1, bit2, bitX : MSB-first within a BE16-refill word.
        //   - bit4             : LSB-first within a BE32-refill word.
        //
        // Mirrors temisu/ancient's MSBBitReader/LSBBitReader classes from
        // src/InputStream.hpp.
        enum class bit_mode { msb_be16, lsb_be32 };

        struct fwd_bit_reader {
            const std::uint8_t* data = nullptr;
            std::size_t* fwd_pos = nullptr;
            std::size_t  bwd_limit = 0;
            std::uint32_t buf = 0;
            unsigned int bits = 0;
            bit_mode mode = bit_mode::msb_be16;
            bool fail = false;

            // Refill the accumulator with one BE16 (MSB mode) or BE32 (LSB
            // mode) word pulled from the shared forward byte stream.
            bool refill() noexcept {
                if (mode == bit_mode::msb_be16) {
                    if (*fwd_pos + 2u > bwd_limit) { fail = true; return false; }
                    buf = (static_cast<std::uint32_t>(data[*fwd_pos])     << 8) |
                           static_cast<std::uint32_t>(data[*fwd_pos + 1]);
                    *fwd_pos += 2u;
                    bits = 16u;
                } else {
                    if (*fwd_pos + 4u > bwd_limit) { fail = true; return false; }
                    buf = (static_cast<std::uint32_t>(data[*fwd_pos])     << 24) |
                          (static_cast<std::uint32_t>(data[*fwd_pos + 1]) << 16) |
                          (static_cast<std::uint32_t>(data[*fwd_pos + 2]) <<  8) |
                           static_cast<std::uint32_t>(data[*fwd_pos + 3]);
                    *fwd_pos += 4u;
                    bits = 32u;
                }
                return true;
            }

            // Read up to `n` bits in the configured mode. Mirrors
            // MSBBitReader::readBitsGeneric (MSB) and
            // LSBBitReader::readBitsGeneric (LSB) byte-for-byte.
            std::uint32_t read_be(unsigned int n) noexcept {
                std::uint32_t out = 0;
                if (mode == bit_mode::msb_be16) {
                    while (n) {
                        if (bits == 0u && !refill()) return 0;
                        const unsigned int take = std::min(n, bits);
                        bits -= take;
                        out = (out << take) |
                              ((buf >> bits) & ((1u << take) - 1u));
                        n -= take;
                    }
                } else {
                    unsigned int pos = 0;
                    while (n) {
                        if (bits == 0u && !refill()) return 0;
                        const unsigned int take = std::min(n, bits);
                        out |= (buf & ((1u << take) - 1u)) << pos;
                        buf >>= take;
                        bits -= take;
                        n -= take;
                        pos += take;
                    }
                }
                return out;
            }
        };

        // Decode a NUKE distance code given a 4-bit prefix index 0..15.
        //
        // The original VLC bit-length list from ancient is:
        //   {4, 6, 8, 9,  -4, 7, 9, 11, 13, 14,  -5, 7, 9, 11, 13, 14}
        // where -4 and -5 mark sub-table boundaries that reset the base
        // back to 0 for the entries that follow. The absolute value of
        // each marker IS itself a valid entry (so |-4| = 4 bits, |-5| = 5
        // bits), giving three sub-tables of 4, 6, and 6 entries (sum 16).
        //
        // Within each sub-table the base accumulates as the sum of
        // (1 << prior_bits). Across sub-tables it resets to 0 — that's
        // the whole point of the markers.
        std::uint32_t decode_nuke_vlc(unsigned int idx, fwd_bit_reader& br) {
            constexpr unsigned int sub_lens[3]   = {4, 6, 6};
            constexpr std::uint8_t bits_table[3][6] = {
                {4, 6, 8, 9, 0, 0},        // sub 0
                {4, 7, 9, 11, 13, 14},     // sub 1: bits=4 from |-4| marker
                {5, 7, 9, 11, 13, 14},     // sub 2: bits=5 from |-5| marker
            };
            unsigned int sub = 0;
            unsigned int local = idx;
            for (unsigned int s = 0; s < 3; ++s) {
                if (local < sub_lens[s]) { sub = s; break; }
                local -= sub_lens[s];
            }
            std::uint32_t base = 0;
            for (unsigned int j = 0; j < local; ++j) {
                base += 1u << bits_table[sub][j];
            }
            const unsigned int bits = bits_table[sub][local];
            const std::uint32_t off = br.read_be(bits);
            return base + off;
        }
    } // namespace

    result xpk_nuke_decompress(
        std::span<const std::uint8_t> packed,
        std::span<std::uint8_t>       raw) {
        if (raw.empty()) return {};
        if (packed.size() < 2u) {
            return make_unexpected("yafa: NUKE chunk too small");
        }
        std::size_t fwd_pos = 0;
        std::size_t bwd_pos = packed.size();
        // Four independent forward bit readers all sharing the same fwd
        // cursor — matches ancient's MSBBitReader/LSBBitReader split.
        // bit1/bit2/bitX use MSB-first within BE16 refill; bit4 is LSB-first
        // within BE32 refill (the only LSB reader in NUKE).
        fwd_bit_reader b1{packed.data(), &fwd_pos, packed.size()};
        fwd_bit_reader b2{packed.data(), &fwd_pos, packed.size()};
        fwd_bit_reader b4{packed.data(), &fwd_pos, packed.size(), 0, 0,
                          bit_mode::lsb_be32};
        fwd_bit_reader bX{packed.data(), &fwd_pos, packed.size()};
        // bwd_limit is the live boundary on the forward side; updated as
        // backward reads pull bytes from the tail.
        auto sync_limits = [&]() {
            b1.bwd_limit = bwd_pos;
            b2.bwd_limit = bwd_pos;
            b4.bwd_limit = bwd_pos;
            bX.bwd_limit = bwd_pos;
        };
        sync_limits();

        auto read_byte_bwd = [&]() -> int {
            if (bwd_pos == 0u || bwd_pos <= fwd_pos) return -1;
            --bwd_pos;
            sync_limits();
            return packed[bwd_pos];
        };

        std::size_t out_pos = 0;
        const std::size_t out_size = raw.size();

        for (;;) {
            const std::uint32_t bit = b1.read_be(1);
            if (b1.fail) return make_unexpected("yafa: NUKE bit1 underrun");
            if (bit == 0u) {
                std::uint32_t count = 0;
                if (b1.read_be(1)) {
                    count = 1;
                } else {
                    for (;;) {
                        const std::uint32_t tmp = b2.read_be(2);
                        if (b2.fail) return make_unexpected("yafa: NUKE bit2 underrun");
                        if (tmp) { count += 5 - tmp; break; }
                        count += 3;
                    }
                }
                for (std::uint32_t i = 0; i < count; ++i) {
                    const int v = read_byte_bwd();
                    if (v < 0)  return make_unexpected("yafa: NUKE literal underrun");
                    if (out_pos >= out_size)
                        return make_unexpected("yafa: NUKE literal output overflow");
                    raw[out_pos++] = static_cast<std::uint8_t>(v);
                }
            }
            if (out_pos >= out_size) break;

            const std::uint32_t idx = b4.read_be(4);
            if (b4.fail) return make_unexpected("yafa: NUKE bit4 underrun");
            const std::uint32_t distance = decode_nuke_vlc(idx, bX);
            if (bX.fail) return make_unexpected("yafa: NUKE vlc underrun");

            std::uint32_t count = 0;
            if (idx < 4u) {
                count = 2;
            } else if (idx < 10u) {
                count = 3;
            } else {
                const std::uint32_t c2 = b2.read_be(2);
                if (b2.fail) return make_unexpected("yafa: NUKE count2 underrun");
                if (c2 == 0u) {
                    count = 3 + 3;
                    for (;;) {
                        const std::uint32_t c4 = b4.read_be(4);
                        if (b4.fail) return make_unexpected("yafa: NUKE count4 underrun");
                        if (c4) { count += 16 - c4; break; }
                        count += 15;
                    }
                } else {
                    count = 3 + 4 - c2;
                }
            }
            if (distance == 0u || distance > out_pos) {
                return make_unexpected("yafa: NUKE distance out of range");
            }
            if (out_pos + count > out_size) {
                return make_unexpected("yafa: NUKE copy output overflow");
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                raw[out_pos] = raw[out_pos - distance];
                ++out_pos;
            }
        }
        return {};
    }

    // ------------------------------------------------------------------------
    // YAFA byte/word/long delta. Per the spec, delta-compressed frames have:
    //   - 8 u32 plane-opcode-stream offsets (32 bytes)
    //   - 8 u32 plane-data-stream offsets   (32 bytes)
    //   - opcode bytes per plane
    //   - data bytes/words/longwords per plane
    //
    // Decompression is per bitplane, splitting each into vertical columns
    // `column_pixels` wide (8 / 16 / 32 for byte / word / long). Within
    // each column the opcode stream encodes:
    //   - skip op (non-zero, top bit clear): advance `op` rows
    //   - uniq op (non-zero, top bit set): copy next `op & 0x7F` items
    //   - same op (zero) followed by count: write count copies of next item
    // Output is applied IN PLACE on top of the previous-frame buffer (COPY
    // semantics, not XOR).
    //
    // Buffer layout: plane k starts at offset k * bytes_per_plane.
    // ------------------------------------------------------------------------
    result yafa_apply_delta(
        std::span<const std::uint8_t> delta,
        std::uint8_t*                 prev,
        unsigned int                  width,
        unsigned int                  height,
        unsigned int                  planes,
        delta_kind                    width_kind) {
        if (delta.size() < 64u) {
            return make_unexpected("yafa: delta too small for plane tables");
        }
        const std::size_t bytes_per_row = (static_cast<std::size_t>(width) + 7u) / 8u;
        const std::size_t bytes_per_plane = bytes_per_row * height;

        unsigned int data_size = 0;     // bytes per data item
        unsigned int column_bytes = 0;  // bytes per row within a column
        switch (width_kind) {
            case delta_kind::byte:  data_size = 1; column_bytes = 1; break;
            case delta_kind::word:  data_size = 2; column_bytes = 2; break;
            case delta_kind::dlong: data_size = 4; column_bytes = 4; break;
            default:
                return make_unexpected("yafa: delta width_kind invalid");
        }

        byte_reader optab{delta.subspan(0, 32)};
        byte_reader dttab{delta.subspan(32, 32)};

        for (unsigned int p = 0; p < std::min(planes, 8u); ++p) {
            std::uint32_t op_off = 0, dt_off = 0;
            optab >> op_off;
            dttab >> dt_off;
            if (op_off == 0u) continue; // unchanged plane
            if (op_off >= delta.size() || dt_off >= delta.size()) {
                return make_unexpected("yafa: delta plane offset past end");
            }
            byte_reader op{delta.subspan(op_off)};
            byte_reader dt{delta.subspan(dt_off)};

            std::uint8_t* plane_base = prev + static_cast<std::size_t>(p) * bytes_per_plane;
            const std::size_t cols = (bytes_per_row + column_bytes - 1u) / column_bytes;

            for (std::size_t c = 0; c < cols; ++c) {
                const auto op_count_v = op.read_u8();
                if (op.failed()) return make_unexpected("yafa: delta op-count truncated");
                if (op_count_v == 0u) continue; // entire column unchanged

                std::uint8_t* col_base =
                    plane_base + c * column_bytes;
                std::size_t row = 0;
                for (unsigned int i = 0; i < op_count_v; ++i) {
                    const auto opv = op.read_u8();
                    if (op.failed()) return make_unexpected("yafa: delta opcode truncated");
                    if (opv == 0u) {
                        // SAME — write `count` copies of the next data item.
                        const auto cnt = op.read_u8();
                        if (op.failed()) return make_unexpected("yafa: delta SAME count truncated");
                        if (!dt.has(data_size)) {
                            return make_unexpected("yafa: delta SAME data truncated");
                        }
                        std::uint8_t buf[4] = {};
                        for (unsigned int b = 0; b < data_size; ++b) buf[b] = dt.read_u8();
                        for (unsigned int n = 0; n < cnt; ++n) {
                            if (row >= height) break;
                            std::uint8_t* dst = col_base + row * bytes_per_row;
                            for (unsigned int b = 0; b < column_bytes; ++b) {
                                if (c * column_bytes + b < bytes_per_row) {
                                    dst[b] = buf[b];
                                }
                            }
                            ++row;
                        }
                    } else if ((opv & 0x80u) == 0u) {
                        // SKIP — advance `opv` rows untouched.
                        row += opv;
                        if (row > height) row = height;
                    } else {
                        // UNIQ — copy `opv & 0x7F` items.
                        const unsigned int cnt = opv & 0x7Fu;
                        if (!dt.has(static_cast<std::size_t>(cnt) * data_size)) {
                            return make_unexpected("yafa: delta UNIQ data truncated");
                        }
                        for (unsigned int n = 0; n < cnt; ++n) {
                            std::uint8_t buf[4] = {};
                            for (unsigned int b = 0; b < data_size; ++b) buf[b] = dt.read_u8();
                            if (row >= height) continue;
                            std::uint8_t* dst = col_base + row * bytes_per_row;
                            for (unsigned int b = 0; b < column_bytes; ++b) {
                                if (c * column_bytes + b < bytes_per_row) {
                                    dst[b] = buf[b];
                                }
                            }
                            ++row;
                        }
                    }
                }
            }
        }
        return {};
    }
} // namespace yafa
