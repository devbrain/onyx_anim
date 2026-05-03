#pragma once

// LSB-first bit reader over a `span<const std::uint8_t>`. Within each
// byte the first bit returned is bit 0 (LSB); when accumulating
// multiple bits via `get_bits(n)`, the first consumed bit becomes the
// LSB of the result.
//
// This convention matches both Smacker (ffmpeg's libavcodec/smacker.c
// uses `#define BITSTREAM_READER_LE`) and Bink (bink.c uses the same
// define). Both RAD codecs emit codes LSB-first within each byte, so
// they share this reader.
//
// Header-only — the inner loop (Huffman walk, residue decode) needs
// to inline for the decoder to be fast. Bounds checks stay off the
// hot path; callers verify `bits_left()` at decision points.
//
// Convenience helpers:
//   peek_bits(n)      — read N bits without advancing the cursor
//   skip_bits(n)      — skip N bits, marking the reader as failed if
//                       this would run past the end
//   byte_align()      — advance to next 8-bit boundary
//   align_to_32()     — advance to next 32-bit boundary (Bink uses
//                       this between planes / blocks)

#include <cstddef>
#include <cstdint>
#include <span>

namespace bytes {
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
                const auto v =
                    (data_[byte_idx] >> static_cast <unsigned int>(bit_pos_ & 7u)) & 1u;
                ++bit_pos_;
                return v;
            }

            // n-bit field; first bit consumed is the LSB of the result.
            // n must be <= 32. Returns 0-padded results on overrun
            // (caller checks bits_left() before driving big reads).
            unsigned int get_bits(unsigned int n) noexcept {
                unsigned int v = 0;
                for (unsigned int i = 0; i < n; ++i) {
                    v |= (get_bit() << i);
                }
                return v;
            }

            // Peek up to 32 bits without consuming. Used by the VLC
            // table lookup: peek the table-bits prefix, consume the
            // actual code length after looking up.
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

            void byte_align() noexcept {
                bit_pos_ = (bit_pos_ + 7u) & ~static_cast <std::size_t>(7u);
            }

            void align_to_32() noexcept {
                bit_pos_ = (bit_pos_ + 31u) & ~static_cast <std::size_t>(31u);
            }

        private:
            std::span <const std::uint8_t> data_;
            std::size_t bit_pos_ = 0;
            bool failed_ = false;
    };
} // namespace bytes
