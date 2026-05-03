#pragma once

// LSB-first bit reader, matching ffmpeg's `#define BITSTREAM_READER_LE` mode
// used in libavcodec/bink.c. Within each byte the first bit returned is bit
// 0 (LSB); when accumulating multiple bits via get_bits(n), the first
// consumed bit becomes the LSB of the result.
//
// Header-only; the inner loop (Huffman walk, residue decode) needs to inline
// for the decoder to be fast enough to be useful. Bounds checks are kept
// off the hot path — callers verify bits_left() at decision points.

#include <cstddef>
#include <cstdint>
#include <span>

namespace bink {
    class bit_reader {
        public:
            explicit bit_reader(std::span <const std::uint8_t> data) noexcept
                : data_(data) {
            }

            [[nodiscard]] bool good() const noexcept { return !failed_; }

            [[nodiscard]] std::size_t bits_left() const noexcept {
                const auto total = data_.size() * 8u;
                return bit_pos_ < total ? total - bit_pos_ : 0;
            }

            [[nodiscard]] std::size_t bits_consumed() const noexcept {
                return bit_pos_;
            }

            // Single bit, LSB-first within each byte.
            unsigned int get_bit() noexcept {
                const auto byte_idx = bit_pos_ >> 3u;
                if (byte_idx >= data_.size()) {
                    failed_ = true;
                    return 0;
                }
                const auto v =
                    (data_[byte_idx] >> static_cast <unsigned int>(bit_pos_ & 7u)) & 1u;
                ++bit_pos_;
                return v;
            }

            // n-bit field; first bit consumed is the LSB of the result.
            // n must be <= 32. Caller should verify bits_left() >= n on the
            // hot path; this routine returns 0-padded results on overrun.
            unsigned int get_bits(unsigned int n) noexcept {
                unsigned int v = 0;
                for (unsigned int i = 0; i < n; ++i) {
                    v |= (get_bit() << i);
                }
                return v;
            }

            // Peek up to 32 bits without consuming. Used by the VLC table
            // lookup: we peek at the table-bits prefix, consume the actual
            // code length after looking up.
            unsigned int peek_bits(unsigned int n) const noexcept {
                std::size_t pos = bit_pos_;
                unsigned int v = 0;
                for (unsigned int i = 0; i < n; ++i) {
                    const auto byte_idx = pos >> 3u;
                    if (byte_idx >= data_.size()) break; // 0-pad
                    const auto bit =
                        (data_[byte_idx] >> static_cast <unsigned int>(pos & 7u)) & 1u;
                    v |= (bit << i);
                    ++pos;
                }
                return v;
            }

            void skip_bits(unsigned int n) noexcept {
                bit_pos_ += n;
                if (bit_pos_ > data_.size() * 8u) failed_ = true;
            }

            // Align cursor to next 32-bit boundary. Bink writes per-plane
            // data flushed to a 32-bit boundary at the end of each plane.
            void align_to_32() noexcept {
                bit_pos_ = (bit_pos_ + 31u) & ~static_cast <std::size_t>(31u);
            }

        private:
            std::span <const std::uint8_t> data_;
            std::size_t bit_pos_ = 0;
            bool failed_ = false;
    };
} // namespace bink
