#include <onyx_anim/codecs/register_codecs.hh>
#include <onyx_anim/sdk/codec_registry.hh>

// Each codec block is gated by ONYX_ANIM_HAS_<NAME>, defined per-axis by
// codecs/CMakeLists.txt. Disabled codecs are absent from the build
// entirely — header, sub-lib, and registration call.

#if defined(ONYX_ANIM_HAS_FLC)
#include "flc.hh"
#endif
#if defined(ONYX_ANIM_HAS_SMK)
#include "smk.hh"
#endif
#if defined(ONYX_ANIM_HAS_ATARI_SEQ)
#include "atari_seq.hh"
#endif
#if defined(ONYX_ANIM_HAS_AMIGA_ANIM)
#include "amiga_anim.hh"
#endif
#if defined(ONYX_ANIM_HAS_CDXL)
#include "cdxl.hh"
#endif
#if defined(ONYX_ANIM_HAS_YAFA)
#include "yafa.hh"
#endif
#if defined(ONYX_ANIM_HAS_DPAN)
#include "dpan.hh"
#endif
#if defined(ONYX_ANIM_HAS_BINK)
#include "bink.hh"
#endif

namespace onyx_anim {
    void register_all_codecs(codec_registry& registry) {
#if defined(ONYX_ANIM_HAS_FLC)
        registry.register_factory([] { return flc_decoder::create(); });
#endif
#if defined(ONYX_ANIM_HAS_SMK)
        registry.register_factory([] { return smk_decoder::create(); });
#endif
#if defined(ONYX_ANIM_HAS_ATARI_SEQ)
        registry.register_factory([] { return atari_seq_decoder::create(); });
#endif
#if defined(ONYX_ANIM_HAS_AMIGA_ANIM)
        registry.register_factory([] { return amiga_anim_decoder::create(); });
#endif
#if defined(ONYX_ANIM_HAS_CDXL)
        registry.register_factory([] { return cdxl_decoder::create(); });
#endif
#if defined(ONYX_ANIM_HAS_YAFA)
        registry.register_factory([] { return yafa_decoder::create(); });
#endif
#if defined(ONYX_ANIM_HAS_DPAN)
        registry.register_factory([] { return dpan_decoder::create(); });
#endif
#if defined(ONYX_ANIM_HAS_BINK)
        registry.register_factory([] { return bink_decoder::create(); });
#endif
        // No-op when all codecs are disabled — registry stays empty.
        (void)registry;
    }
} // namespace onyx_anim
