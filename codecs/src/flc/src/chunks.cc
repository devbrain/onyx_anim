#include <flc/chunks.hh>

namespace flc {
    expected <sub_chunk_header>
    parse_sub_chunk_header(std::span <const std::uint8_t> data) {
        if (data.size() < kSubChunkHeaderSize) {
            return make_unexpected("flc: sub-chunk header truncated");
        }
        byte_reader br{data};
        sub_chunk_header h{};
        std::uint16_t type_raw = 0;
        br >> h.size >> type_raw;
        h.type = static_cast <sub_chunk_type>(type_raw);
        if (h.size < kSubChunkHeaderSize) {
            return make_unexpected("flc: sub-chunk size smaller than header");
        }
        return h;
    }

    expected <std::span <const std::uint8_t>>
    sub_chunk_payload(std::span <const std::uint8_t> chunk_with_header) {
        auto h = parse_sub_chunk_header(chunk_with_header);
        if (!h) {
            return make_unexpected(h.error());
        }
        if (chunk_with_header.size() < h->size) {
            return make_unexpected("flc: sub-chunk payload truncated");
        }
        return chunk_with_header.subspan(kSubChunkHeaderSize,
                                         h->size - kSubChunkHeaderSize);
    }
} // namespace flc
