#include <bink/tree.hh>

#include <algorithm>
#include <cstring>

namespace bink {
    namespace {
        // Merge step from ffmpeg's bink.c::merge — interleave two equal-
        // length lists into `dst` according to a bit-stream-controlled
        // selector, then drain whichever side has leftovers.
        void merge_lists(bit_reader& br,
                         std::uint8_t* dst,
                         const std::uint8_t* src, int size) {
            const std::uint8_t* src1 = src;
            const std::uint8_t* src2 = src + size;
            int s1 = size, s2 = size;

            while (s1 > 0 && s2 > 0) {
                if (br.get_bit() == 0) {
                    *dst++ = *src1++; --s1;
                } else {
                    *dst++ = *src2++; --s2;
                }
            }
            while (s1-- > 0) *dst++ = *src1++;
            while (s2-- > 0) *dst++ = *src2++;
        }
    } // namespace

    expected<tree> read_tree(bit_reader& br) {
        tree t{};
        if (br.bits_left() < 4) {
            return make_unexpected<error_type>("bink: tree-vlc-num truncated");
        }
        t.vlc_num = br.get_bits(4);
        if (t.vlc_num == 0) {
            for (unsigned int i = 0; i < 16; ++i) {
                t.syms[i] = static_cast <std::uint8_t>(i);
            }
            return t;
        }

        if (br.bits_left() < 1) {
            return make_unexpected<error_type>("bink: tree variant bit truncated");
        }
        if (br.get_bit() != 0u) {
            // Literal-list variant. `len`+1 explicit 4-bit symbols, then
            // ascending fill. `tmp1[]` doubles as the "already used"
            // bitmap — set when an explicit symbol has been emitted.
            std::uint8_t tmp1[16] = {0};
            if (br.bits_left() < 3u) {
                return make_unexpected<error_type>("bink: tree len truncated");
            }
            unsigned int len = br.get_bits(3);
            if (br.bits_left() < (static_cast <std::size_t>(len) + 1u) * 4u) {
                return make_unexpected<error_type>("bink: tree literals truncated");
            }
            for (unsigned int i = 0; i <= len; ++i) {
                t.syms[i] = static_cast <std::uint8_t>(br.get_bits(4));
                tmp1[t.syms[i]] = 1;
            }
            for (unsigned int i = 0; i < 16 && len < 15; ++i) {
                if (!tmp1[i]) t.syms[++len] = static_cast <std::uint8_t>(i);
            }
        } else {
            // Merge-tower variant. Build identity, then repeatedly merge
            // adjacent runs of size (1, 2, 4, ...) until reaching the
            // configured depth. Two ping-pong scratch buffers.
            std::uint8_t tmp1[16], tmp2[16];
            for (unsigned int i = 0; i < 16; ++i) tmp1[i] = static_cast <std::uint8_t>(i);
            std::uint8_t* in = tmp1;
            std::uint8_t* out = tmp2;
            if (br.bits_left() < 2u) {
                return make_unexpected<error_type>("bink: tree merge-len truncated");
            }
            const int len = static_cast <int>(br.get_bits(2));
            for (int i = 0; i <= len; ++i) {
                const int size = 1 << i;
                for (int s = 0; s < 16; s += size << 1) {
                    merge_lists(br, out + s, in + s, size);
                }
                std::swap(in, out);
            }
            std::memcpy(t.syms.data(), in, 16);
        }
        return t;
    }
} // namespace bink
