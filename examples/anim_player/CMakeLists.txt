# anim_player — SDL3 + Dear ImGui front-end for onyx_anim.
#
# Pulls in Dear ImGui via FetchContent, builds it together with the SDL3
# and SDL_Renderer backends as a static library, then links a small
# executable that decodes frames through onyx_anim and displays them.

include(FetchContent)

set(IMGUI_VERSION v1.91.5)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${IMGUI_VERSION}
    GIT_SHALLOW    TRUE
    UPDATE_COMMAND ""
)
FetchContent_MakeAvailable(imgui)

# SDL3 target (made available transitively by musac).
if (TARGET SDL3::SDL3)
    set(_SDL3_TARGET SDL3::SDL3)
elseif (TARGET SDL3-static)
    set(_SDL3_TARGET SDL3-static)
else ()
    message(WARNING "anim_player: SDL3 target not found; skipping")
    return ()
endif ()

# Build imgui + SDL3/SDL_Renderer3 backends as a single static lib.
add_library(anim_player_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(anim_player_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(anim_player_imgui PUBLIC ${_SDL3_TARGET})
set_target_properties(anim_player_imgui PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

add_executable(anim_player anim_player.cc)
target_link_libraries(anim_player PRIVATE
    neutrino::onyx_anim_sdk
    neutrino::onyx_anim_codecs
    neutrino::onyx_image
    anim_player_imgui
    ${_SDL3_TARGET}
    musac
    musac_backend_sdl3
    musac_codecs
)
target_compile_features(anim_player PRIVATE cxx_std_20)
