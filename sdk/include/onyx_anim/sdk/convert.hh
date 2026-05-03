#pragma once

#include <onyx_anim/sdk/onyx_anim_sdk_export.h>
#include <onyx_anim/sdk/types.hh>

#include <onyx_image/surface.hpp>

namespace onyx_anim {
    /**
     * Convert pixels from a `memory_surface` (the codec's internal output
     * buffer) into an arbitrary engine-supplied `surface` (typically a
     * write-only texture upload sink). Picks the most compact path that
     * yields the requested `dst_format`:
     *
     *   indexed8 → indexed8       palette + raw indices, no expansion
     *   indexed8 → rgb888         palette-expand each pixel to 3 bytes
     *   indexed8 → rgba8888       palette-expand + opaque alpha (0xFF)
     *   rgb888   → rgb888         pass-through pixel rows
     *   rgb888   → rgba8888       interleave alpha=0xFF
     *   rgba8888 → rgba8888       pass-through pixel rows
     *
     * Other directions (downconversion to indexed8, dropping alpha) are
     * not currently supported — they require quantisation that is best
     * left to engines. Returns an error when invoked on an unsupported
     * direction.
     *
     * `dst.set_size(...)` is called before any pixel writes. For the
     * indexed8 destination case we additionally call
     * `dst.set_palette_size(...)` and `dst.write_palette(...)` so
     * downstream renderers can pick up the palette.
     *
     * `src` must be a fully-decoded frame: its width / height / pixel
     * data are read directly. The conversion does not allocate inside
     * `dst` — the engine's `surface` subclass decides how to allocate /
     * lock its backing.
     */
    [[nodiscard]] ONYX_ANIM_SDK_EXPORT result
    convert_surface(const onyx_image::memory_surface& src,
                    onyx_image::surface& dst,
                    pixel_format dst_format);
} // namespace onyx_anim
