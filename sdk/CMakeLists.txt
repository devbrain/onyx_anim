# ============================================================================
# onyx_anim_sdk — abstract animation decoder interface
# ============================================================================

if (ONYX_ANIM_BUILD_SHARED)
    set(_lib_type SHARED)
else ()
    set(_lib_type STATIC)
endif ()

add_library(onyx_anim_sdk ${_lib_type})
add_library(neutrino::onyx_anim_sdk ALIAS onyx_anim_sdk)

target_compile_features(onyx_anim_sdk PUBLIC cxx_std_20)

target_include_directories(onyx_anim_sdk
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(onyx_anim_sdk
        PUBLIC
        neutrino::onyx_image
        neutrino::musac
        tl::expected
)

generate_export_header(onyx_anim_sdk
        BASE_NAME onyx_anim_sdk
        EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/onyx_anim/sdk/onyx_anim_sdk_export.h
        STATIC_DEFINE ONYX_ANIM_SDK_STATIC_DEFINE
)

if (NOT ONYX_ANIM_BUILD_SHARED)
    target_compile_definitions(onyx_anim_sdk PUBLIC ONYX_ANIM_SDK_STATIC_DEFINE)
endif ()

set_target_properties(onyx_anim_sdk PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        POSITION_INDEPENDENT_CODE ON
)

target_sources(onyx_anim_sdk PRIVATE
        src/codec_registry.cc
        src/decoder.cc
        src/convert.cc
        src/frame_clock.cc
        src/probe.cc
)
