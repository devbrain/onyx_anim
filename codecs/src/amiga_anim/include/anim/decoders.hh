#pragma once

#include <anim/header.hh>
#include <anim/types.hh>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace anim {
    /**
     * Decompress ByteRun1 (PackBits) from a stream into a fixed-size buffer.
     * Returns the number of source bytes consumed on success.
     *
     * Op codes (per Amiga IFF spec):
     *   n in [0, 127]   : copy n+1 bytes verbatim
     *   n in [-127, -1] : repeat the next byte (-n)+1 times
     *   n == -128       : no-op (skip)
     */
    [[nodiscard]] expected<std::size_t>
        unpack_byterun1(std::span<const std::uint8_t> src,
                        std::uint8_t* dst, std::size_t expected);

    // All apply_dlta_* functions operate on a row-interleaved planar
    // framebuffer where each row stores plane 0's bpr bytes, then plane 1's
    // bpr bytes, ..., then plane N-1's bpr bytes (total `bpr * planes` per
    // row). They match the corresponding decoders in ffmpeg's libavcodec/iff.c.

    /// Op 3 — Short Horizontal Delta. Position-offset/value records.
    [[nodiscard]] result
        apply_dlta_op3(std::span<const std::uint8_t> dlta,
                       std::uint8_t* fb,
                       unsigned int  width,
                       unsigned int  planes,
                       std::size_t   fb_size);

    /// Op 5 — Byte Vertical Delta (COPY semantics for animations; XOR mode is
    /// reserved for brush usage signaled by ANHD `bits == 2`).
    [[nodiscard]] result
        apply_dlta_op5(std::span<const std::uint8_t> dlta,
                       std::uint8_t* fb,
                       std::size_t   bytes_per_row,
                       unsigned int  planes,
                       unsigned int  height);

    /// Op 7 short (16-bit Vertical Delta — opcodes and 16-bit data live in
    /// separate per-plane streams within the chunk).
    [[nodiscard]] result
        apply_dlta_op7_short(std::span<const std::uint8_t> dlta,
                             std::uint8_t* fb,
                             unsigned int  width,
                             unsigned int  planes,
                             std::size_t   fb_size);

    /// Op 7 long (32-bit Vertical Delta).
    [[nodiscard]] result
        apply_dlta_op7_long(std::span<const std::uint8_t> dlta,
                            std::uint8_t* fb,
                            unsigned int  width,
                            unsigned int  planes,
                            std::size_t   fb_size);

    /// Op 8 short — like op 7 short but with 16-bit opcodes inline (no
    /// separate data table) and 16-bit-only writes.
    [[nodiscard]] result
        apply_dlta_op8_short(std::span<const std::uint8_t> dlta,
                             std::uint8_t* fb,
                             unsigned int  width,
                             unsigned int  planes,
                             std::size_t   fb_size);

    /// Op 8 long — 32-bit variant of op 8 short.
    [[nodiscard]] result
        apply_dlta_op8_long(std::span<const std::uint8_t> dlta,
                            std::uint8_t* fb,
                            unsigned int  width,
                            unsigned int  planes,
                            std::size_t   fb_size);

    /// Op J — type-tagged record stream with column-group / row-group writes.
    [[nodiscard]] result
        apply_dlta_opj(std::span<const std::uint8_t> dlta,
                       std::uint8_t* fb,
                       unsigned int  width,
                       unsigned int  height,
                       unsigned int  planes,
                       std::size_t   fb_size);

    /// Op L (0x6C / 108) — per-plane (offset, count) stream. Two 8-longword
    /// tables at the head of the chunk (offsets 0 and 32) hold word-offsets
    /// (×2 for byte position) into the data and opcode streams respectively.
    /// Each (cnt < 0) record runs one 16-bit datum |cnt| times; (cnt > 0)
    /// emits `cnt` literal 16-bit words. Stream terminates on peek == 0xFFFF.
    /// `is_short` selects the write stride: true → row-pitch (vertical run),
    /// false → 2 (horizontal run). Maps to ANHD `bits & 1 == 0`.
    [[nodiscard]] result
        apply_dlta_op_l(std::span<const std::uint8_t> dlta,
                        std::uint8_t* fb,
                        unsigned int  width,
                        unsigned int  planes,
                        bool          is_short,
                        std::size_t   fb_size);

    /// Op D (0x64 / 100) — Scala/InfoChannel ANIM32. Header is `planes`
    /// longwords of per-plane byte-offsets into the chunk. Each plane
    /// stream begins with a u32 `entries` count, followed by `entries`
    /// records of (s32 opcode, u32 offset). `offset` is a byte position
    /// within the plane; the write address is computed identically to op
    /// L. opcode > 0 → run of `opcode` copies of one u32 value; opcode < 0
    /// → `|opcode|` literal u32 values. Writes step by full row pitch.
    /// Caller must reject the interlaced flag.
    [[nodiscard]] result
        apply_dlta_op_d(std::span<const std::uint8_t> dlta,
                        std::uint8_t* fb,
                        unsigned int  width,
                        unsigned int  planes,
                        std::size_t   fb_size);

    /// Op E (0x65 / 101) — Scala/InfoChannel ANIM16. Same shape as op D
    /// but `entries` is u16, opcode is s16, and the run/literal payload
    /// is u16. Writes step by full row pitch. Caller must reject the
    /// interlaced flag.
    [[nodiscard]] result
        apply_dlta_op_e(std::span<const std::uint8_t> dlta,
                        std::uint8_t* fb,
                        unsigned int  width,
                        unsigned int  planes,
                        std::size_t   fb_size);

    /**
     * Convert row-interleaved planar storage (the layout used both by ILBM
     * BODY chunks and by ANIM op-5 DLTA framebuffers) to chunky 8-bit
     * indexed pixels.
     *
     * Per row: `planes * bytes_per_row` bytes hold plane 0's row first,
     * then plane 1's row, ..., then plane N-1's row. The next row begins
     * at `+ planes * bytes_per_row`.
     */
    void planar_interleaved_to_chunky(const std::uint8_t* planar,
                                      std::size_t bytes_per_row,
                                      unsigned int planes,
                                      std::uint8_t* chunky,
                                      std::size_t chunky_stride,
                                      unsigned int width,
                                      unsigned int height) noexcept;
} // namespace anim
