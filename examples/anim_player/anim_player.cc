// anim_player — minimal SDL3 + Dear ImGui front-end for onyx_anim.
//
// Decodes one animation file to memory_surface frames, converts each to
// RGBA32, and displays through SDL_Renderer with an ImGui control panel
// (play/pause, frame slider, loop toggle, info, drag-and-drop reload).

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <onyx_anim/sdk/codec_registry.hh>
#include <onyx_anim/sdk/decoder.hh>
#include <onyx_anim/codecs/register_codecs.hh>
#include <onyx_image/surface.hpp>

#include <musac/audio_device.hh>
#include <musac/audio_source.hh>
#include <musac/audio_system.hh>
#include <musac/stream.hh>
#include <musac/codecs/register_codecs.hh>
#include <musac/sdk/decoder.hh>
#include <musac/sdk/decoders_registry.hh>
#include <musac/sdk/io_stream.hh>
#include <musac_backends/sdl3/sdl3_backend.hh>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

struct anim_state {
    std::unique_ptr<musac::io_stream>           stream;
    std::unique_ptr<onyx_anim::anim_decoder>    decoder;
    onyx_image::memory_surface                  surf;
    std::vector<std::uint8_t>                   rgba; // converted current frame
    SDL_Texture*                                texture = nullptr;
    int                                         tex_w = 0;
    int                                         tex_h = 0;
    unsigned int                                frame_index = 0;
    bool                                        playing = true;
    bool                                        loop = true;
    double                                      accumulator_us = 0.0;
    std::string                                 source_path;
    std::string                                 status; // last error or info text

    // Audio: optional musac stream tied to the demuxer's first audio track.
    // Lifetime is bounded by anim_state — destroying the stream stops audio.
    std::optional<musac::audio_stream>          audio_stream;

    // Per-frame event-driven audio (ANIM+SLA): one short-lived stream per
    // SCTL trigger. Streams that have finished are pruned each tick. Capped
    // to keep runaway repeat-loops from exhausting the audio device.
    std::vector<musac::audio_stream>            event_streams;
    static constexpr std::size_t                kMaxEventStreams = 16;
};

void destroy_texture(anim_state& a) {
    if (a.texture) {
        SDL_DestroyTexture(a.texture);
        a.texture = nullptr;
    }
}

bool ensure_texture(anim_state& a, SDL_Renderer* r, int w, int h) {
    if (a.texture && a.tex_w == w && a.tex_h == h) return true;
    destroy_texture(a);
    a.texture = SDL_CreateTexture(r,
                                  SDL_PIXELFORMAT_RGBA32,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  w, h);
    if (!a.texture) return false;
    SDL_SetTextureScaleMode(a.texture, SDL_SCALEMODE_NEAREST);
    a.tex_w = w;
    a.tex_h = h;
    return true;
}

// Convert the decoded frame in `a.surf` to RGBA32 in `a.rgba`.
void surface_to_rgba(anim_state& a) {
    const int w = a.surf.width();
    const int h = a.surf.height();
    a.rgba.assign(static_cast<std::size_t>(w) * h * 4u, 0);
    const auto px = a.surf.pixels();
    if (a.surf.format() == onyx_image::pixel_format::rgb888) {
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* src = px.data() + y * w * 3;
            std::uint8_t*       dst = a.rgba.data() + y * w * 4;
            for (int x = 0; x < w; ++x) {
                dst[x * 4 + 0] = src[x * 3 + 0];
                dst[x * 4 + 1] = src[x * 3 + 1];
                dst[x * 4 + 2] = src[x * 3 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
        return;
    }
    // indexed8 + palette
    const auto pal = a.surf.palette();
    const std::size_t pal_entries = pal.size() / 3u;
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* src = px.data() + y * w;
        std::uint8_t*       dst = a.rgba.data() + y * w * 4;
        for (int x = 0; x < w; ++x) {
            std::size_t idx = src[x];
            if (idx >= pal_entries) idx = 0;
            dst[x * 4 + 0] = pal[idx * 3 + 0];
            dst[x * 4 + 1] = pal[idx * 3 + 1];
            dst[x * 4 + 2] = pal[idx * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }
}

void upload_current_frame(anim_state& a, SDL_Renderer* r) {
    if (a.surf.width() <= 0 || a.surf.height() <= 0) return;
    surface_to_rgba(a);
    if (!ensure_texture(a, r, a.surf.width(), a.surf.height())) return;
    SDL_UpdateTexture(a.texture, nullptr,
                      a.rgba.data(),
                      a.surf.width() * 4);
}

void fire_pending_audio_events(anim_state& a, musac::audio_device* device);

// Build a tiny dummy io_stream for audio_source. Our decoder ignores the
// io_stream (it owns a pre-loaded PCM buffer) but audio_source::open()
// requires a non-null one.
std::unique_ptr<musac::io_stream> dummy_io_stream() {
    static const std::uint8_t kEmpty[1] = {0};
    return musac::io_from_memory(kEmpty, sizeof(kEmpty));
}

void start_audio_track(anim_state& a, musac::audio_device* device) {
    a.audio_stream.reset(); // stop/destroy any previous stream
    if (!device || !a.decoder) return;
    if (a.decoder->audio_track_count() == 0u) return;
    auto track = a.decoder->take_audio_track(0u);
    if (!track) return;
    try {
        musac::audio_source src{std::move(track), dummy_io_stream()};
        a.audio_stream.emplace(device->create_stream(std::move(src)));
        a.audio_stream->play();
    } catch (const std::exception& ex) {
        a.status = std::string("audio init failed: ") + ex.what();
        a.audio_stream.reset();
    }
}

bool open_path(anim_state& a, SDL_Renderer* r,
               musac::audio_device* device, const char* path) {
    // Drop existing streams BEFORE the old decoder goes away — the audio
    // threads read through state we'd be destroying. event_streams are
    // independent of the decoder (they own their own io+decoder) but we
    // tear them down anyway since they belong to the previous animation.
    a.audio_stream.reset();
    a.event_streams.clear();

    a.stream = musac::io_from_file(path, "rb");
    if (!a.stream) {
        a.status = std::string("cannot open: ") + path;
        return false;
    }
    auto& reg = onyx_anim::codec_registry::instance();
    a.decoder = reg.create_decoder(a.stream.get());
    if (!a.decoder) {
        a.status = "no codec accepted this file";
        return false;
    }
    if (auto rc = a.decoder->open(a.stream.get()); !rc) {
        a.status = std::string("open failed: ") + rc.error();
        return false;
    }
    a.source_path    = path;
    a.frame_index    = 0;
    a.accumulator_us = 0.0;
    a.playing        = true;
    auto fr = a.decoder->decode_frame(a.surf);
    if (!fr) {
        a.status = std::string("decode_frame failed: ") + fr.error();
        return false;
    }
    upload_current_frame(a, r);
    a.status = path;
    start_audio_track(a, device);
    fire_pending_audio_events(a, device);
    return true;
}

// Spawn a short-lived musac stream for each event triggered by the
// just-decoded frame (event-driven audio, e.g. ANIM+SLA SCTL chunks).
// Drops finished streams to keep the active set bounded.
void fire_pending_audio_events(anim_state& a, musac::audio_device* device) {
    if (!device || !a.decoder) return;

    // Prune streams that have finished playing.
    std::erase_if(a.event_streams,
                  [](const musac::audio_stream& s) { return !s.is_playing(); });

    const auto events = a.decoder->pending_audio_events();
    for (const auto& ev : events) {
        if (!ev.sound_bytes || ev.sound_bytes->empty()) continue;
        if (a.event_streams.size() >= anim_state::kMaxEventStreams) break;
        try {
            auto io = musac::io_from_memory(ev.sound_bytes->data(),
                                            ev.sound_bytes->size());
            // Format-agnostic: musac auto-detects via the global decoders
            // registry installed at audio_system::init(). The player stays
            // codec-neutral; new container types just need their decoder
            // registered with musac.
            musac::audio_source src{std::move(io)};
            auto stream = device->create_stream(std::move(src));
            // SCTL.repeats == 0 means loop forever; map to 0 for musac too.
            const int iterations = ev.repeats == 0u ? 0 : ev.repeats;
            stream.play(iterations, std::chrono::microseconds{});
            a.event_streams.push_back(std::move(stream));
        } catch (const std::exception&) {
            // Best-effort: a malformed sample shouldn't kill playback.
        }
    }
}

bool advance_one(anim_state& a, SDL_Renderer* r,
                 musac::audio_device* device) {
    if (!a.decoder) return false;
    if (a.decoder->eof()) {
        if (!a.loop) {
            a.playing = false;
            // Audio stream will naturally finish when its buffer drains.
            return false;
        }
        if (!a.decoder->rewind()) {
            a.status = "rewind failed";
            a.playing = false;
            return false;
        }
        a.frame_index = 0;
        // rewind() atomically reset the audio cursor back to 0; if the audio
        // stream had already stopped at end-of-buffer, kick it back into life.
        if (a.audio_stream && !a.audio_stream->is_playing()) {
            a.audio_stream->play();
        }
    }
    auto fr = a.decoder->decode_frame(a.surf);
    if (!fr) {
        a.status = std::string("decode error: ") + fr.error();
        a.playing = false;
        return false;
    }
    a.frame_index = fr->index;
    upload_current_frame(a, r);
    fire_pending_audio_events(a, device);
    return true;
}

void seek_to(anim_state& a, SDL_Renderer* r, unsigned int idx) {
    if (!a.decoder) return;
    if (!a.decoder->seek_to_frame(idx)) {
        a.status = "seek failed";
        return;
    }
    auto fr = a.decoder->decode_frame(a.surf);
    if (!fr) { a.status = std::string("decode after seek: ") + fr.error(); return; }
    a.frame_index = fr->index;
    upload_current_frame(a, r);
    a.accumulator_us = 0.0;
    // The decoder atomically rewrote the audio cursor inside seek_to_frame;
    // if the audio stream had stopped (end-of-stream after a previous play
    // through), re-arm playback so it picks up at the new cursor.
    if (a.audio_stream && !a.audio_stream->is_playing()) {
        a.audio_stream->play();
    }
}

void set_playing(anim_state& a, bool play) {
    a.playing = play;
    if (a.audio_stream) {
        if (play) {
            if (!a.audio_stream->is_playing()) a.audio_stream->play();
            else                               a.audio_stream->resume();
        } else {
            a.audio_stream->pause();
        }
    }
    // Event-driven streams are short-lived one-shots: pause stops them,
    // resume just lets new triggers fire on subsequent decoded frames.
    for (auto& s : a.event_streams) {
        if (play) s.resume();
        else      s.pause();
    }
}

} // namespace

int main(int argc, char* argv[]) {
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("onyx_anim player",
                                     1024, 720,
                                     SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    onyx_anim::register_all_codecs(onyx_anim::codec_registry::instance());

    // Audio: optional. If musac fails (e.g. no audio device on a CI box),
    // keep going with video only. We install a registry pre-populated with
    // every musac codec so format-agnostic audio_source(io_stream) can
    // auto-detect anything an anim_decoder hands us via audio events.
    std::shared_ptr<musac::audio_backend>      audio_backend{};
    std::shared_ptr<musac::decoders_registry>  audio_registry{};
    std::optional<musac::audio_device>         audio_device{};
    try {
        audio_backend  = std::shared_ptr<musac::audio_backend>(
            musac::create_sdl3_backend());
        audio_registry = musac::create_registry_with_all_codecs();
        if (musac::audio_system::init(audio_backend, audio_registry)) {
            audio_device.emplace(
                musac::audio_device::open_default_device(audio_backend));
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr,
                     "audio init failed (continuing without audio): %s\n",
                     ex.what());
        audio_device.reset();
    }
    musac::audio_device* device = audio_device ? &*audio_device : nullptr;

    anim_state st;
    if (argc >= 2) {
        if (!open_path(st, renderer, device, argv[1])) {
            std::fprintf(stderr, "%s\n", st.status.c_str());
        }
    } else {
        st.status = "drop a .flc/.seq/.anim file on the window to load";
    }

    auto last_tick = std::chrono::steady_clock::now();
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            switch (e.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    if (e.window.windowID == SDL_GetWindowID(window))
                        running = false;
                    break;
                case SDL_EVENT_DROP_FILE:
                    if (e.drop.data) open_path(st, renderer, device, e.drop.data);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.key == SDLK_SPACE) set_playing(st, !st.playing);
                    if (e.key.key == SDLK_R)     {
                        if (st.decoder) {
                            seek_to(st, renderer, 0);
                            set_playing(st, true);
                        }
                    }
                    break;
                default: break;
            }
        }

        const auto now      = std::chrono::steady_clock::now();
        const auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  now - last_tick).count();
        last_tick = now;

        if (st.playing && st.decoder) {
            const auto period_us = st.decoder->info().frame_period.count();
            if (period_us > 0) {
                st.accumulator_us += static_cast<double>(delta_us);
                while (st.playing && st.accumulator_us >= static_cast<double>(period_us)) {
                    st.accumulator_us -= static_cast<double>(period_us);
                    if (!advance_one(st, renderer, device)) break;
                }
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // ---- Controls window ----
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 220), ImGuiCond_FirstUseEver);
        ImGui::Begin("anim_player");

        if (st.decoder) {
            const auto& info = st.decoder->info();
            ImGui::Text("File:   %s",
                        st.source_path.empty() ? "-" : st.source_path.c_str());
            ImGui::Text("Codec:  %.*s",
                        static_cast<int>(st.decoder->name().size()),
                        st.decoder->name().data());
            ImGui::Text("Size:   %ux%u   Frames: %u",
                        info.width, info.height, info.frame_count);
            const double fps = info.frame_period.count() > 0
                ? 1'000'000.0 / static_cast<double>(info.frame_period.count())
                : 0.0;
            ImGui::Text("Period: %lld us  (%.2f fps)",
                        static_cast<long long>(info.frame_period.count()), fps);
            if (info.audio_track_count > 0u) {
                ImGui::Text("Audio:  %u Hz, %u ch%s",
                            info.audio_rate, info.audio_channels,
                            st.audio_stream ? "" : " (no device)");
            } else {
                ImGui::TextDisabled("Audio:  none");
            }

            ImGui::Separator();

            if (ImGui::Button(st.playing ? "Pause" : "Play")) {
                set_playing(st, !st.playing);
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart")) {
                seek_to(st, renderer, 0);
                set_playing(st, true);
            }
            ImGui::SameLine();
            ImGui::Checkbox("Loop", &st.loop);

            int current = static_cast<int>(st.frame_index);
            const int last = (info.frame_count > 0)
                ? static_cast<int>(info.frame_count - 1) : 0;
            if (ImGui::SliderInt("Frame", &current, 0, last)) {
                seek_to(st, renderer, static_cast<unsigned int>(current));
                set_playing(st, false);
            }
        } else {
            ImGui::TextWrapped("No file loaded.");
            ImGui::TextWrapped("Drop a .flc / .seq / .anim file onto the window,");
            ImGui::TextWrapped("or pass it on the command line.");
        }

        if (!st.status.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", st.status.c_str());
        }
        ImGui::End();

        // ---- Draw frame ----
        SDL_SetRenderDrawColor(renderer, 24, 24, 24, 255);
        SDL_RenderClear(renderer);

        if (st.texture) {
            int win_w = 0, win_h = 0;
            SDL_GetRenderOutputSize(renderer, &win_w, &win_h);
            const float fw    = static_cast<float>(st.tex_w);
            const float fh    = static_cast<float>(st.tex_h);
            const float fwin_w = static_cast<float>(win_w);
            const float fwin_h = static_cast<float>(win_h);
            const float scale = std::min(fwin_w / fw, fwin_h / fh);
            const float dw    = fw * scale;
            const float dh    = fh * scale;
            SDL_FRect dst{
                (fwin_w - dw) * 0.5f,
                (fwin_h - dh) * 0.5f,
                dw, dh
            };
            SDL_RenderTexture(renderer, st.texture, nullptr, &dst);
        }

        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // Destroy all audio streams + decoder before tearing down the audio
    // device (musac stream lifetime must end before its parent device).
    st.event_streams.clear();
    st.audio_stream.reset();
    st.decoder.reset();
    audio_device.reset();
    musac::audio_system::done();

    destroy_texture(st);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
