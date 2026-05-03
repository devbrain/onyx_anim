# ============================================================================
# onyx_anim_codecs — built-in FLC and SMK implementations
# ============================================================================

# Fetch failsafe for codec-internal logging/contract checks. No-op if a
# parent project (e.g. via musac) has already provided the target.
include(${NEUTRINO_CMAKE_DIR}/deps/failsafe.cmake)
neutrino_fetch_failsafe()

if (ONYX_ANIM_BUILD_SHARED)
    set(_lib_type SHARED)
else ()
    set(_lib_type STATIC)
endif ()

add_library(onyx_anim_codecs ${_lib_type})
add_library(neutrino::onyx_anim_codecs ALIAS onyx_anim_codecs)

target_compile_features(onyx_anim_codecs PUBLIC cxx_std_20)

target_include_directories(onyx_anim_codecs
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(onyx_anim_codecs
        PUBLIC
        neutrino::onyx_anim_sdk
        PRIVATE
        lib_flc
        lib_atari_seq
        lib_amiga_anim
        lib_cdxl
        lib_yafa
        neutrino::failsafe
)

generate_export_header(onyx_anim_codecs
        BASE_NAME onyx_anim_codecs
        EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/onyx_anim/codecs/onyx_anim_codecs_export.h
        STATIC_DEFINE ONYX_ANIM_CODECS_STATIC_DEFINE
)

if (NOT ONYX_ANIM_BUILD_SHARED)
    target_compile_definitions(onyx_anim_codecs PUBLIC ONYX_ANIM_CODECS_STATIC_DEFINE)
endif ()

set_target_properties(onyx_anim_codecs PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        POSITION_INDEPENDENT_CODE ON
)

target_sources(onyx_anim_codecs PRIVATE
        src/flc.cc
        src/smk.cc
        src/atari_seq.cc
        src/amiga_anim.cc
        src/cdxl.cc
        src/yafa.cc
        src/register_codecs.cc
)

add_subdirectory(src/bytes)
add_subdirectory(src/flc)
add_subdirectory(src/atari_seq)
add_subdirectory(src/amiga_anim)
add_subdirectory(src/cdxl)
add_subdirectory(src/yafa)
