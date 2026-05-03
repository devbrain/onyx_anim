// anim_player — minimal SDL3 + Dear ImGui front-end for onyx_anim,
// built on top of `onyx_anim::player` to show how slim an engine
// integration becomes.

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <onyx_anim/codecs/register_codecs.hh>
#include <onyx_anim/player/player.hh>
#include <onyx_anim/sdk/codec_registry.hh>

#include <onyx_image/surface.hpp>

#include <musac/audio_device.hh>
#include <musac/audio_system.hh>
#include <musac/codecs/register_codecs.hh>
#include <musac/sdk/decoders_registry.hh>
#include <musac/sdk/io_stream.hh>
#include <musac/stream.hh>
#include <musac_backends/sdl3/sdl3_backend.hh>

#include <algorithm>
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
    // RGBA-only surface backed by a vector of bytes. The player asks us
    // for rgba8888; we hand it this and then upload directly into the
    // SDL streaming texture — no second copy.
    class rgba_surface final : public onyx_image::surface {
        public:
            bool set_size(int w, int h,
                          onyx_image::pixel_format) override {
                width_ = w;
                height_ = h;
                pixels_.assign(static_cast<std::size_t>(w) * h * 4u, 0);
                return true;
            }

            void write_pixels(int x, int y, int count,
                              const std::uint8_t* src) override {
                if (y < 0 || y >= height_ || x < 0 || count <= 0) return;
                const std::size_t row_off =
                    static_cast<std::size_t>(y) * width_ * 4u + x;
                if (row_off + static_cast<std::size_t>(count) > pixels_.size()) return;
                std::memcpy(pixels_.data() + row_off, src,
                            static_cast<std::size_t>(count));
            }

            void write_pixel(int, int, std::uint8_t) override {
                // The player guarantees rgba8888, so the indexed-pixel
                // path is never taken — no-op.
            }

            int width() const noexcept { return width_; }
            int height() const noexcept { return height_; }
            const std::uint8_t* data() const noexcept { return pixels_.data(); }

        private:
            int width_ = 0;
            int height_ = 0;
            std::vector<std::uint8_t> pixels_;
    };

    struct app_state {
        std::unique_ptr<onyx_anim::player>  player;
        rgba_surface                        frame;
        SDL_Texture*                        texture = nullptr;
        int                                 tex_w = 0;
        int                                 tex_h = 0;
        bool                                loop = true;
        std::string                         source_path;
        std::string                         status;
    };

    void destroy_texture(app_state& a) {
        if (a.texture) {
            SDL_DestroyTexture(a.texture);
            a.texture = nullptr;
        }
    }

    bool ensure_texture(app_state& a, SDL_Renderer* r, int w, int h) {
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

    void upload_frame(app_state& a, SDL_Renderer* r) {
        const int w = a.frame.width();
        const int h = a.frame.height();
        if (w <= 0 || h <= 0) return;
        if (!ensure_texture(a, r, w, h)) return;
        SDL_UpdateTexture(a.texture, nullptr, a.frame.data(), w * 4);
    }

    bool open_path(app_state& a, SDL_Renderer* r,
                   musac::audio_device* device, const char* path) {
        // Drop the previous player BEFORE the audio stream it owns goes
        // away — order matters because the audio thread reads through
        // state owned by player_impl.
        a.player.reset();

        auto stream = musac::io_from_file(path, "rb");
        if (!stream) {
            a.status = std::string("cannot open: ") + path;
            return false;
        }

        onyx_anim::player_options opts;
        opts.loop = a.loop;
        opts.audio_device = device;
        opts.preferred_format = onyx_image::pixel_format::rgba8888;

        auto pr = onyx_anim::player::open(std::move(stream), opts);
        if (!pr) {
            a.status = std::string("player::open: ") + pr.error();
            return false;
        }
        a.player = std::move(*pr);
        a.player->on_error([&a](const std::string& e) { a.status = e; });
        a.player->play();

        a.source_path = path;
        a.status = path;

        // Render the first frame immediately. The player automatically
        // fires any event-driven audio (ANIM+SLA SCTL) attached to this
        // frame because we passed an audio_device in the options.
        if (a.player->advance_to_time(std::chrono::microseconds{0}, a.frame)) {
            upload_frame(a, r);
        }
        return true;
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

    app_state st;
    if (argc >= 2) {
        if (!open_path(st, renderer, device, argv[1])) {
            std::fprintf(stderr, "%s\n", st.status.c_str());
        }
    } else {
        st.status = "drop a .flc/.smk/.bik/... file on the window to load";
    }

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
                    if (e.key.key == SDLK_SPACE && st.player) {
                        if (st.player->is_playing()) st.player->pause();
                        else                          st.player->play();
                    }
                    if (e.key.key == SDLK_R && st.player) {
                        st.player->rewind();
                        st.player->play();
                    }
                    break;
                default: break;
            }
        }

        // Tick the player. Returns true when a fresh frame landed in
        // st.frame; in that case we just re-upload the texture. Audio
        // (streaming + event-driven) is handled internally because we
        // passed an audio_device in the player options.
        if (st.player) {
            if (st.player->tick(st.frame)) {
                upload_frame(st, renderer);
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // ---- Controls window ----
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 220), ImGuiCond_FirstUseEver);
        ImGui::Begin("anim_player");

        if (st.player) {
            const auto& info = st.player->info();
            ImGui::Text("File:   %s",
                        st.source_path.empty() ? "-" : st.source_path.c_str());
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
                            st.player->audio_stream() ? "" : " (no device)");
            } else {
                ImGui::TextDisabled("Audio:  none");
            }

            ImGui::Separator();

            const bool playing = st.player->is_playing();
            if (ImGui::Button(playing ? "Pause" : "Play")) {
                if (playing) st.player->pause();
                else         st.player->play();
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart")) {
                st.player->rewind();
                st.player->play();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Loop", &st.loop);

            // Frame slider derived from current_time / frame_period.
            const auto period = info.frame_period;
            int current = period.count() > 0
                ? static_cast<int>(st.player->current_time().count() / period.count())
                : 0;
            const int last = (info.frame_count > 0)
                ? static_cast<int>(info.frame_count - 1) : 0;
            if (ImGui::SliderInt("Frame", &current, 0, last)) {
                st.player->seek_to_time(period * static_cast<std::int64_t>(current));
                st.player->pause();
                if (st.player->advance_to_time(
                        period * static_cast<std::int64_t>(current), st.frame)) {
                    upload_frame(st, renderer);
                }
            }
        } else {
            ImGui::TextWrapped("No file loaded.");
            ImGui::TextWrapped("Drop a video file onto the window,");
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
            const float fw     = static_cast<float>(st.tex_w);
            const float fh     = static_cast<float>(st.tex_h);
            const float fwin_w = static_cast<float>(win_w);
            const float fwin_h = static_cast<float>(win_h);
            const float scale  = std::min(fwin_w / fw, fwin_h / fh);
            const float dw     = fw * scale;
            const float dh     = fh * scale;
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

    // Tear down in reverse construction order — player owns the audio
    // stream pulling from the audio_device, so it must die first.
    st.player.reset();
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
