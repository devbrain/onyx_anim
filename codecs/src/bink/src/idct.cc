#include <bink/idct.hh>

#include <algorithm>
#include <cstring>

namespace bink {
    namespace {
        // Fixed-point IDCT constants (Q11). Identical to ffmpeg's binkdsp.c.
        constexpr std::int32_t A1 =  2896;  // (1/sqrt(2)) << 12
        constexpr std::int32_t A2 =  2217;
        constexpr std::int32_t A3 =  3784;
        constexpr std::int32_t A4 = -5352;

        // ffmpeg's MUL macro: (int)((unsigned)X * Y) >> 11. The trick is
        // that the multiply runs in u32 modular arithmetic (so signed
        // overflow is well-defined) and is then cast back to int32 to
        // get an arithmetic right shift. Doing `>> 11` on the u32 first
        // would be a logical shift and would mishandle negative
        // products.
        constexpr std::int32_t mul11(std::int32_t x, std::int32_t y) noexcept {
            const auto prod_u =
                static_cast <std::uint32_t>(x) *
                static_cast <std::uint32_t>(y);
            return static_cast <std::int32_t>(prod_u) >> 11;
        }

        // The IDCT_TRANSFORM macro from ffmpeg, written as a templated
        // function so it can do both row + column passes with different
        // strides and "munge" (rounding) functions.
        template <int Stride, typename Munge>
        inline void idct_transform(std::int32_t* dst, const std::int32_t* src,
                                   Munge munge) noexcept {
            const std::int32_t a0 = src[0]            + src[4 * Stride];
            const std::int32_t a1 = src[0]            - src[4 * Stride];
            const std::int32_t a2 = src[2 * Stride]   + src[6 * Stride];
            const std::int32_t a3 = mul11(A1, src[2 * Stride] - src[6 * Stride]);
            const std::int32_t a4 = src[5 * Stride]   + src[3 * Stride];
            const std::int32_t a5 = src[5 * Stride]   - src[3 * Stride];
            const std::int32_t a6 = src[1 * Stride]   + src[7 * Stride];
            const std::int32_t a7 = src[1 * Stride]   - src[7 * Stride];
            const std::int32_t b0 = a4 + a6;
            const std::int32_t b1 = mul11(A3, a5 + a7);
            const std::int32_t b2 = mul11(A4, a5) - b0 + b1;
            const std::int32_t b3 = mul11(A1, a6 - a4) - b2;
            const std::int32_t b4 = mul11(A2, a7) + b3 - b1;

            dst[0 * Stride] = munge(a0 + a2     + b0);
            dst[1 * Stride] = munge(a1 + a3 - a2 + b2);
            dst[2 * Stride] = munge(a1 - a3 + a2 + b3);
            dst[3 * Stride] = munge(a0 - a2     - b4);
            dst[4 * Stride] = munge(a0 - a2     + b4);
            dst[5 * Stride] = munge(a1 - a3 + a2 - b3);
            dst[6 * Stride] = munge(a1 + a3 - a2 - b2);
            dst[7 * Stride] = munge(a0 + a2     - b0);
        }

        struct munge_none {
            std::int32_t operator()(std::int32_t x) const noexcept { return x; }
        };
        struct munge_row {
            // Row pass produces final pixel values; rounded by (x + 0x7F) >> 8.
            std::int32_t operator()(std::int32_t x) const noexcept {
                return (x + 0x7F) >> 8;
            }
        };

        // Column pass: input has stride 8 (next row), output has stride 8.
        void idct_col(std::int32_t* dst, const std::int32_t* src) noexcept {
            // Fast-path: column is constant in rows 1..7 → broadcast row 0.
            if ((src[8] | src[16] | src[24] | src[32] | src[40] | src[48] | src[56]) == 0) {
                const std::int32_t v = src[0];
                dst[0] = dst[8] = dst[16] = dst[24] = dst[32] = dst[40] = dst[48] = dst[56] = v;
                return;
            }
            idct_transform<8>(dst, src, munge_none{});
        }
    } // namespace

    void idct_put(std::uint8_t* dst, std::size_t stride, std::int32_t* block) noexcept {
        std::int32_t tmp[64];
        for (int i = 0; i < 8; ++i) idct_col(&tmp[i], &block[i]);
        for (int i = 0; i < 8; ++i) {
            // Inline row pass writing directly to dst with row munge.
            const std::int32_t* s = &tmp[8 * i];
            std::int32_t row[8];
            idct_transform<1>(row, s, munge_row{});
            std::uint8_t* o = dst + static_cast <std::size_t>(i) * stride;
            for (int j = 0; j < 8; ++j) {
                o[j] = static_cast <std::uint8_t>(row[j] & 0xFF);
            }
        }
    }

    void idct_add(std::uint8_t* dst, std::size_t stride, std::int32_t* block) noexcept {
        // ffmpeg's idct_add_c: run the IDCT in-place into block, then add
        // each value to dst, with the documented wraparound (no clamp).
        std::int32_t tmp[64];
        for (int i = 0; i < 8; ++i) idct_col(&tmp[i], &block[i]);
        for (int i = 0; i < 8; ++i) {
            std::int32_t* row_dst = &block[8 * i];
            idct_transform<1>(row_dst, &tmp[8 * i], munge_row{});
        }
        for (int i = 0; i < 8; ++i) {
            std::uint8_t* o = dst + static_cast <std::size_t>(i) * stride;
            for (int j = 0; j < 8; ++j) {
                o[j] = static_cast <std::uint8_t>(o[j] + block[i * 8 + j]);
            }
        }
    }

    void scale_block(const std::uint8_t* src, std::uint8_t* dst,
                     std::size_t stride) noexcept {
        // Produce 16x16 from 8x8 by replicating each pixel into a 2x2.
        for (int j = 0; j < 8; ++j) {
            std::uint8_t* d1 = dst;
            std::uint8_t* d2 = dst + stride;
            for (int i = 0; i < 8; ++i) {
                d1[2 * i]     = src[i];
                d1[2 * i + 1] = src[i];
                d2[2 * i]     = src[i];
                d2[2 * i + 1] = src[i];
            }
            src += 8;
            dst += stride * 2;
        }
    }

    void add_pixels8(std::uint8_t* dst, std::int16_t* block,
                     std::size_t stride) noexcept {
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 8; ++j) {
                dst[j] = static_cast <std::uint8_t>(dst[j] + block[j]);
            }
            dst += stride;
            block += 8;
        }
    }
} // namespace bink
