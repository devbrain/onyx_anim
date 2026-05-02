#pragma once

#include <onyx_anim/codecs/onyx_anim_codecs_export.h>

namespace onyx_anim {
    class codec_registry;

    /**
     * Register all built-in animation decoders (FLC, SMK) with the given
     * registry. Mirrors musac::register_all_codecs.
     */
    ONYX_ANIM_CODECS_EXPORT void register_all_codecs(codec_registry& registry);
} // namespace onyx_anim
