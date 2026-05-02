#include <onyx_anim/codecs/register_codecs.hh>
#include <onyx_anim/sdk/codec_registry.hh>

#include "flc.hh"
#include "smk.hh"
#include "atari_seq.hh"
#include "amiga_anim.hh"

namespace onyx_anim {
    void register_all_codecs(codec_registry& registry) {
        registry.register_factory([] { return flc_decoder::create(); });
        registry.register_factory([] { return smk_decoder::create(); });
        registry.register_factory([] { return atari_seq_decoder::create(); });
        registry.register_factory([] { return amiga_anim_decoder::create(); });
    }
} // namespace onyx_anim
