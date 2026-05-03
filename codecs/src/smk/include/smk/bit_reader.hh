#pragma once

// LSB-first bit reader, matching ffmpeg's `#define BITSTREAM_READER_LE` mode
// which Smacker uses (see libavcodec/smacker.c). Within each byte the first
// bit returned by get_bit() is bit 0 (LSB); when accumulating multiple bits
// via get_bits(n), the first consumed bit becomes the LSB of the result.
//
// Consequences for SMK:
//   - get_bits(8)        returns the next byte unchanged.
//   - get_bits(16)       returns the LE u16 of the next 2 bytes.
//   - bit-by-bit Huffman walk reads the bit stream in the order the
//     encoder emitted it (Smacker's small-tree / big-tree paths).
//
// Header-only; the inner loop has to inline to be tolerable for the
// bigtree walk.

#include <cstddef>
#include <cstdint>
#include <span>

namespace smk {
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

            // Single bit, LSB-first within each byte. No bounds check —
            // caller must verify bits_left() >= 1.
            unsigned int get_bit() noexcept {
                const auto byte_idx = bit_pos_ >> 3u;
                if (byte_idx >= data_.size()) {
                    failed_ = true;
                    return 0;
                }
                const auto bit_in_byte =
                    static_cast <unsigned int>(bit_pos_ & 7u);
                const auto v =
                    (data_[byte_idx] >> bit_in_byte) & 1u;
                ++bit_pos_;
                return v;
            }

            // n-bit field; first bit consumed is the LSB of the result.
            // n must be <= 32.
            unsigned int get_bits(unsigned int n) noexcept {
                unsigned int v = 0;
                for (unsigned int i = 0; i < n; ++i) {
                    v |= (get_bit() << i);
                }
                return v;
            }

            void skip_bits(unsigned int n) noexcept {
                bit_pos_ += n;
                if (bit_pos_ > data_.size() * 8u) failed_ = true;
            }

            // Convenience: align cursor to next byte boundary.
            void byte_align() noexcept {
                bit_pos_ = (bit_pos_ + 7u) & ~static_cast <std::size_t>(7u);
            }

        private:
            std::span <const std::uint8_t> data_;
            std::size_t bit_pos_ = 0;
            bool failed_ = false;
    };
} // namespace smk
