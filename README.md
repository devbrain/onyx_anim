# onyx_anim

Animation and video codec library for the Neutrino ecosystem, aimed at game
engines and tools that need to play classic animation containers without
pulling in a full multimedia stack.

C++20, no exceptions on the hot path, abstract surface I/O so engines plug
their own renderer in.

## Supported formats

| Container             | Video                              | Audio                          | Cross-checked against     |
|-----------------------|------------------------------------|--------------------------------|---------------------------|
| FLI / FLC             | All standard chunks                | none                           | ffmpeg `flicvideo.c`      |
| Atari SEQ             | Run-length frames                  | none                           | Randelshofer reference    |
| Amiga ANIM            | Op 3, 5, 7s, 7l, 8s, 8l, J, 108    | AnimFX SBDY, ANIM+SLA SCTL     | ffmpeg `iff.c`            |
| CDXL                  | Bit-planar                         | Raw 8-bit signed PCM           | ffmpeg `cdxl.c`           |
| YAFA                  | Bit-planar                         | none                           | (in-tree fixtures)        |
| Deluxe Paint ANM      | Frame-delta                        | none                           | ffmpeg `anm.c`            |
| Smacker (SMK2 / SMK4) | Full Huffman tree set              | DPCM + 16-bit truncated        | ffmpeg `smacker.c`        |
| Bink1 (BIKi / BIKb)   | YV12 / 4:2:0                       | Bink Audio (DCT and RDFT)      | ffmpeg `bink.c`           |

## Layout

The project ships as three sub-libraries so consumers only link what they
need.

- `neutrino::onyx_anim_sdk` — abstract `anim_decoder` interface, codec
  registry, `audio_track_info`, `convert_surface()`, `frame_clock`,
  `probe()`. No codec code, no audio dependency.
- `neutrino::onyx_anim_codecs` — every supported codec, registered through
  `register_all_codecs()`.
- `neutrino::onyx_anim_player` — engine-facing tick-per-frame `player`
  class, `cpu_surface` helper. Pulls in [musac](https://github.com/devbrain/musac)
  for audio.

A typical engine integration links all three.

## Quickstart

### Probe a file

```cpp
#include <onyx_anim/codecs/register_codecs.hh>
#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/probe.hh>

onyx_anim::register_all_codecs(onyx_anim::codec_registry::instance());

auto stream = musac::io_from_file("intro.bik", "rb");
auto info = onyx_anim::probe(stream.get());
if (!info) { /* handle error */ }

std::printf("%s, %ux%u, %u frames\n",
            info->codec_name.c_str(),
            info->video.width, info->video.height,
            info->video.frame_count);
```

### Tick once per game frame

```cpp
#include <onyx_anim/player/cpu_surface.hh>
#include <onyx_anim/player/player.hh>

onyx_anim::player_options opts;
opts.audio_device     = &my_musac_device;   // optional
opts.preferred_format = onyx_image::pixel_format::rgba8888;
opts.loop             = true;

auto p = onyx_anim::player::open(std::move(stream), opts).value();
p->play();

onyx_anim::cpu_surface frame;
while (running) {
    if (p->tick(frame)) {
        engine.upload_texture(frame.data(), frame.width(),
                              frame.height(), frame.pitch());
    }
    engine.render();
}
```

When `audio_device` is set, the player owns the audio_stream, drives its
own clock from the audio thread's byte cursor (no drift), and auto-fires
event-driven one-shots like ANIM+SLA SCTL triggers. Engines that want full
control take the audio source out via `take_audio_track()` and adopt their
own `audio_stream` back via `adopt_audio_stream()`.

### Engine-side surfaces

`cpu_surface` is the drop-in path for engines without zero-copy streaming
texture mapping. For zero-copy paths (Vulkan staging buffers, SDL streaming
textures with `SDL_LockTexture`, GL persistent maps) implement
`onyx_image::surface` directly — the player never allocates inside it.

## Build

Standalone:

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CMake options:

- `NEUTRINO_ONYX_ANIM_BUILD_SHARED` — build sub-libraries as shared.
- `NEUTRINO_ONYX_ANIM_BUILD_TESTS` — enable doctests and (when ffmpeg /
  Randelshofer's `seqconverter.jar` are available) format cross-checks.
- `NEUTRINO_ONYX_ANIM_BUILD_EXAMPLES` — build `anim_player` (SDL3 +
  Dear ImGui front-end) and the small CLI tools.

As a consumer:

```cmake
FetchContent_Declare(onyx_anim
    GIT_REPOSITORY https://github.com/<your-org>/onyx_anim.git
    GIT_TAG        main)
FetchContent_MakeAvailable(onyx_anim)

target_link_libraries(my_engine PRIVATE
    neutrino::onyx_anim_sdk
    neutrino::onyx_anim_codecs
    neutrino::onyx_anim_player)
```

## Examples

- `examples/anim_player` — SDL3 + Dear ImGui player, drag-and-drop, frame
  scrubber, ~360 lines all in. Reference for engine integration.
- `examples/onyx_anim_info` — `probe()` driver, prints codec / resolution
  / frame rate / per-track audio metadata.
- `examples/anim_to_ppm`, `examples/flc_to_ppm`, `examples/seq_to_ppm` —
  CLI frame dumpers used by the cross-check tests.
- `examples/anim_dump_audio` — splits the first audio track to raw PCM.

## Cross-checks

When ffmpeg is on the PATH (and Randelshofer's `seqconverter.jar` is in
`~/Downloads/` for SEQ), CTest registers one decode-and-bit-compare per
fixture across the entire `data/` corpus. These run as part of `ctest`
and gate every code change. The Bink cross-check compares YUV planes via
a roundtrip through ffmpeg's RGB→YUV conversion, since the codec is
bit-exact at the YUV level but PPM-level comparison is sensitive to YUV→RGB
rounding.

## Dependencies

Fetched automatically via CMake `FetchContent`:

- [neutrino-cmake](https://github.com/devbrain/neutrino-cmake) — shared
  CMake plumbing.
- [onyx_image](https://github.com/devbrain/onyx_image) — abstract surface
  + pixel format types.
- [musac](https://github.com/devbrain/musac) — audio engine; pulls SDL3
  transitively.
- [PocketFFT](https://gitlab.mpcdf.mpg.de/mtr/pocketfft) — RDFT / DCT
  transforms for Bink Audio.
- [tl::expected](https://github.com/TartanLlama/expected) — fallback when
  the toolchain has no `<expected>`.
