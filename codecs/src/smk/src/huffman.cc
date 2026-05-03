#include <smk/huffman.hh>

#include <utility>

namespace smk {
    namespace {
        // Recursion safety caps mirroring ffmpeg's SMKTREE_DECODE_MAX_RECURSION
        // (small tree, 32 levels) and SMKTREE_DECODE_BIG_MAX_RECURSION (big
        // tree, 500 levels). The big tree can grow deep because escape leaves
        // and value-array padding push the depth past what a balanced 16-bit
        // alphabet would need.
        constexpr unsigned int kSmallTreeMaxDepth = 32;
        constexpr unsigned int kBigTreeMaxDepth   = 500;

        // ---------- small tree builder ----------------------------------

        // Recursive descent matching ffmpeg's smacker_decode_tree. Returns
        // the count of cells appended to `out` (== size of this subtree).
        // On failure, sets `failed`.
        std::size_t build_small_recursive(bit_reader& br,
                                          std::vector <std::uint32_t>& out,
                                          unsigned int depth,
                                          bool& failed) {
            if (depth > kSmallTreeMaxDepth) { failed = true; return 0; }
            if (br.bits_left() < 1) { failed = true; return 0; }
            const auto bit = br.get_bit();
            if (bit == 0) {
                // leaf: 8-bit value
                if (br.bits_left() < 8) { failed = true; return 0; }
                const auto v = br.get_bits(8) & 0xFFu;
                out.push_back(v);
                return 1;
            }
            // internal: insert placeholder, recurse left, patch jump,
            // recurse right.
            const auto self = out.size();
            out.push_back(0); // placeholder
            const auto left = build_small_recursive(br, out, depth + 1, failed);
            if (failed) return 0;
            // After left subtree, `out.size() - self - 1 == left`.
            // ffmpeg stores `r` (= left subtree size); the walker advances
            // by that many cells before the post-bit increment, so right
            // subtree starts at `self + left + 1` — matches `out.size()`.
            out[self] = kSmkNodeBit | static_cast <std::uint32_t>(left);
            const auto right = build_small_recursive(br, out, depth + 1, failed);
            if (failed) return 0;
            return left + right + 1;
        }
    } // namespace

    // -------------------------------------------------------------------

    namespace {
        // Shared body of read_small_tree / read_small_tree_bare: do the
        // recursive build then the 1-bit trailer + the n<=1 fold-to-
        // constant. Caller has already established that a tree is present
        // (header-path) or always present (audio-path).
        expected<small_tree>
        finish_small_tree(bit_reader& br) {
            small_tree t{};
            t.present = true;
            bool failed = false;
            const auto n = build_small_recursive(br, t.nodes, 0, failed);
            if (failed) {
                return make_unexpected<error_type>("smk: malformed small tree");
            }
            if (br.bits_left() < 1) {
                return make_unexpected<error_type>("smk: small-tree trailer truncated");
            }
            br.skip_bits(1);
            if (n <= 1) {
                t.has_single = true;
                t.single_value = static_cast <std::uint8_t>(
                    t.nodes.empty() ? 0u : t.nodes[0]);
            } else {
                t.has_single = false;
            }
            return t;
        }
    } // namespace

    expected<small_tree> read_small_tree(bit_reader& br) {
        if (br.bits_left() < 1) {
            return make_unexpected<error_type>("smk: small-tree present-bit truncated");
        }
        if (br.get_bit() == 0) {
            small_tree t{};
            t.present     = false;
            t.has_single  = true;
            t.single_value = 0;
            return t;
        }
        return finish_small_tree(br);
    }

    expected<small_tree> read_small_tree_bare(bit_reader& br) {
        return finish_small_tree(br);
    }

    expected<std::uint8_t> small_tree_decode(const small_tree& t,
                                             bit_reader& br) {
        if (t.has_single) {
            return t.single_value;
        }
        const std::uint32_t* table = t.nodes.data();
        const std::uint32_t* const end = table + t.nodes.size();
        while ((*table & kSmkNodeBit) != 0u) {
            if (br.bits_left() < 1) {
                return make_unexpected<error_type>("smk: bits exhausted in small-tree walk");
            }
            if (br.get_bit() != 0u) {
                table += (*table) & (~kSmkNodeBit);
            }
            ++table;
            if (table >= end) {
                return make_unexpected<error_type>("smk: small-tree walk overran");
            }
        }
        return static_cast <std::uint8_t>(*table & 0xFFu);
    }

    // -------------------------------------------------------------------
    // big tree
    // -------------------------------------------------------------------

    namespace {
        struct big_build_ctx {
            bit_reader&                br;
            const small_tree&          v0; // low byte
            const small_tree&          v1; // high byte
            std::array <std::uint32_t, 3> escapes;
            std::array <int, 3>&       last;
            std::vector <std::uint32_t>& out;
            std::size_t                cap;
            bool                       failed = false;
        };

        // Recursive bigtree builder. Returns the size of this subtree.
        std::size_t build_big_recursive(big_build_ctx& ctx, unsigned int depth) {
            if (depth > kBigTreeMaxDepth) {
                ctx.failed = true;
                return 0;
            }
            if (ctx.out.size() >= ctx.cap) {
                ctx.failed = true;
                return 0;
            }
            if (ctx.br.bits_left() < 1) {
                ctx.failed = true;
                return 0;
            }
            const auto bit = ctx.br.get_bit();
            if (bit == 0) {
                // Leaf: read i1 from low-byte tree, i2 from high-byte tree.
                auto i1 = small_tree_decode(ctx.v0, ctx.br);
                if (!i1) { ctx.failed = true; return 0; }
                auto i2 = small_tree_decode(ctx.v1, ctx.br);
                if (!i2) { ctx.failed = true; return 0; }
                std::uint32_t val = static_cast <std::uint32_t>(*i1) |
                                    (static_cast <std::uint32_t>(*i2) << 8u);
                if (val == ctx.escapes[0]) {
                    ctx.last[0] = static_cast <int>(ctx.out.size());
                    val = 0;
                } else if (val == ctx.escapes[1]) {
                    ctx.last[1] = static_cast <int>(ctx.out.size());
                    val = 0;
                } else if (val == ctx.escapes[2]) {
                    ctx.last[2] = static_cast <int>(ctx.out.size());
                    val = 0;
                }
                ctx.out.push_back(val);
                return 1;
            }
            // Internal node.
            const auto self = ctx.out.size();
            ctx.out.push_back(0); // placeholder
            const auto left = build_big_recursive(ctx, depth + 1);
            if (ctx.failed) return 0;
            ctx.out[self] = kSmkNodeBit | static_cast <std::uint32_t>(left);
            const auto right = build_big_recursive(ctx, depth + 1);
            if (ctx.failed) return 0;
            return left + right + 1;
        }
    } // namespace

    expected<big_tree> read_big_tree(bit_reader& br,
                                     std::uint32_t expected_size_bytes) {
        big_tree t{};

        if (br.bits_left() < 1) {
            return make_unexpected<error_type>("smk: big-tree present-bit truncated");
        }
        if (br.get_bit() == 0) {
            // Skipped → constant 0. ffmpeg allocates a tiny 2-entry table
            // and points all 3 last[] indices at cell 1 so resets become
            // no-ops. We mirror with a 2-entry array.
            t.present = false;
            t.nodes   = {0u, 0u};
            t.last    = {1, 1, 1};
            t.single_value = 0;
            return t;
        }
        t.present = true;

        // Two nested small trees for low/high byte.
        auto lo = read_small_tree(br);
        if (!lo) return make_unexpected<error_type>(lo.error());
        auto hi = read_small_tree(br);
        if (!hi) return make_unexpected<error_type>(hi.error());

        // Three 16-bit escape codepoints.
        if (br.bits_left() < 48) {
            return make_unexpected<error_type>("smk: big-tree escapes truncated");
        }
        // Bytes are LE on disk, MSB-first within each byte. Per ffmpeg this
        // is `get_bits(gb, 16)`, which treats the 16 bits as one big-endian
        // integer. Smacker stored the escape codepoints in that exact form
        // so we just read them MSB-first and use the value as-is — they
        // are compared against `i1 | (i2 << 8)` produced by the same
        // bit-stream, so any consistent reading round-trips.
        for (auto& e : t.escapes) e = br.get_bits(16);

        const std::size_t cap = (static_cast <std::size_t>(expected_size_bytes) + 3u) >> 2u;
        if (cap == 0u || cap > (1u << 24u)) {
            return make_unexpected<error_type>("smk: big-tree size out of range");
        }
        t.nodes.reserve(cap + 3u);

        big_build_ctx ctx{br, *lo, *hi, t.escapes, t.last, t.nodes, cap, false};
        (void) build_big_recursive(ctx, 0);
        if (ctx.failed) {
            return make_unexpected<error_type>("smk: malformed big tree");
        }

        if (br.bits_left() < 1) {
            return make_unexpected<error_type>("smk: big-tree trailer truncated");
        }
        br.skip_bits(1);

        // Materialise any escape that the build never saw — append a
        // placeholder cell whose value is permanently 0.
        for (std::size_t i = 0; i < 3; ++i) {
            if (t.last[i] < 0) {
                t.last[i] = static_cast <int>(t.nodes.size());
                t.nodes.push_back(0);
            }
        }
        return t;
    }

    void big_tree_reset_last(big_tree& t) noexcept {
        for (std::size_t i = 0; i < 3; ++i) {
            const auto idx = t.last[i];
            if (idx >= 0 && static_cast <std::size_t>(idx) < t.nodes.size()) {
                t.nodes[static_cast <std::size_t>(idx)] = 0;
            }
        }
    }

    expected<std::uint32_t> big_tree_decode(big_tree& t, bit_reader& br) {
        if (t.nodes.empty()) {
            return make_unexpected<error_type>("smk: big tree empty");
        }
        std::uint32_t* base = t.nodes.data();
        std::uint32_t* const end = base + t.nodes.size();
        std::uint32_t* table = base;
        while ((*table & kSmkNodeBit) != 0u) {
            if (br.bits_left() < 1) {
                return make_unexpected<error_type>("smk: bits exhausted in big-tree walk");
            }
            if (br.get_bit() != 0u) {
                table += (*table) & (~kSmkNodeBit);
            }
            ++table;
            if (table >= end) {
                return make_unexpected<error_type>("smk: big-tree walk overran");
            }
        }
        const std::uint32_t v = *table;
        // Update recency: if the leaf value differs from the cell at
        // last[0], shift the trio down and put `v` at last[0]. The cells
        // referenced by last[] are themselves leaf cells whose `value`
        // field is what gets read on subsequent decodes — that's the
        // mechanism for "repeat last code".
        const auto l0 = t.last[0];
        const auto l1 = t.last[1];
        const auto l2 = t.last[2];
        if (l0 >= 0 && static_cast <std::size_t>(l0) < t.nodes.size() &&
            base[l0] != v) {
            if (l2 >= 0 && static_cast <std::size_t>(l2) < t.nodes.size() &&
                l1 >= 0 && static_cast <std::size_t>(l1) < t.nodes.size()) {
                base[l2] = base[l1];
                base[l1] = base[l0];
            }
            base[l0] = v;
        }
        return v;
    }
} // namespace smk
