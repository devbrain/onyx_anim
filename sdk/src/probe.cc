#include <onyx_anim/sdk/probe.hh>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>

namespace onyx_anim {
    namespace {
        // RAII guard that restores a stream's cursor to its starting
        // position when it goes out of scope. probe() needs this so a
        // failed open in the middle of header parsing doesn't leave the
        // caller's stream pointed at garbage.
        struct cursor_guard {
            musac::io_stream* s;
            std::int64_t pos;
            ~cursor_guard() { if (s) (void) s->seek(pos, musac::seek_origin::set); }
        };
    } // namespace

    expected<probe_info, error_type> probe(musac::io_stream* stream) {
        if (!stream) return make_unexpected<error_type>("probe: null stream");

        const auto start = stream->tell();
        if (start < 0) return make_unexpected<error_type>("probe: stream is not seekable");
        cursor_guard guard{stream, start};

        auto& reg = codec_registry::instance();
        auto dec = reg.create_decoder(stream);
        if (!dec) {
            return make_unexpected<error_type>("probe: no codec accepted the stream");
        }

        if (auto r = dec->open(stream); !r) {
            return make_unexpected<error_type>(r.error());
        }

        probe_info out;
        out.codec_name = std::string(dec->name());
        out.video      = dec->info();

        const auto n = dec->audio_track_count();
        out.audio_tracks.reserve(n);
        for (unsigned int i = 0; i < n; ++i) {
            out.audio_tracks.push_back(dec->audio_track(i));
        }
        return out;
    }
} // namespace onyx_anim
