# ============================================================================
# onyx_anim_player — high-level engine-facing video player
#
# Wraps onyx_anim_sdk's anim_decoder with timing, audio integration via
# musac, and surface format conversion to give game engines a "tick once
# per frame" API. Independent of any specific codec: depends only on the
# SDK and musac. Engines link this lib in addition to onyx_anim_codecs
# (which registers the actual codec decoders).
# ============================================================================

if (ONYX_ANIM_BUILD_SHARED)
    set(_lib_type SHARED)
else ()
    set(_lib_type STATIC)
endif ()

add_library(onyx_anim_player ${_lib_type})
add_library(neutrino::onyx_anim_player ALIAS onyx_anim_player)

target_compile_features(onyx_anim_player PUBLIC cxx_std_20)

target_include_directories(onyx_anim_player
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(onyx_anim_player
        PUBLIC
        neutrino::onyx_anim_sdk
        neutrino::onyx_image
        neutrino::musac
)

generate_export_header(onyx_anim_player
        BASE_NAME onyx_anim_player
        EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/onyx_anim/player/onyx_anim_player_export.h
        STATIC_DEFINE ONYX_ANIM_PLAYER_STATIC_DEFINE
)

if (NOT ONYX_ANIM_BUILD_SHARED)
    target_compile_definitions(onyx_anim_player PUBLIC ONYX_ANIM_PLAYER_STATIC_DEFINE)
endif ()

set_target_properties(onyx_anim_player PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        POSITION_INDEPENDENT_CODE ON
)

target_sources(onyx_anim_player PRIVATE
        src/player.cc
)
