#include <bink/vlc.hh>
#include <bink/data.hh>

#include <mutex>

namespace bink {
    namespace {
        vlc_tables_t g_tables;
        std::once_flag g_init;

        // Build one tree's lookup table. For each (sym, code C, length L):
        // every 7-bit index whose low L bits == C maps to (sym, L). The
        // remaining (kVlcTableBits - L) high bits don't constrain the
        // lookup — they're whatever comes next in the bit stream.
        void build_tree(unsigned int tree_idx) {
            auto& tab = g_tables[tree_idx];
            // Clear with a sentinel (len=0 means "invalid lookup" — should
            // never happen for a well-formed tree but acts as a poison
            // value if a malformed file feeds bits no symbol covers).
            for (auto& e : tab) e = vlc_entry{0, 0};

            const auto& bits = data::bink_tree_bits[tree_idx];
            const auto& lens = data::bink_tree_lens[tree_idx];

            for (unsigned int sym = 0; sym < 16; ++sym) {
                const auto L = lens[sym];
                const auto C = bits[sym];
                if (L == 0 || L > kVlcTableBits) continue; // skip pathological
                const unsigned int step = 1u << L;
                for (unsigned int idx = C;
                     idx < (1u << kVlcTableBits);
                     idx += step) {
                    tab[idx] = vlc_entry{static_cast <std::uint8_t>(sym),
                                         static_cast <std::uint8_t>(L)};
                }
            }
        }

        void init_tables() {
            for (unsigned int i = 0; i < 16; ++i) build_tree(i);
        }
    } // namespace

    const vlc_tables_t& vlc_tables() noexcept {
        std::call_once(g_init, init_tables);
        return g_tables;
    }
} // namespace bink
