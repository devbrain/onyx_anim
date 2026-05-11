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

# Sub-libs are linked in PRIVATE — they're an implementation detail of
# onyx_anim_codecs. Each per-codec axis controls whether its sub-lib
# gets built (add_subdirectory below) and linked. Disabled codecs are
# completely absent from the build graph.
target_link_libraries(onyx_anim_codecs
        PUBLIC
        neutrino::onyx_anim_sdk
        PRIVATE
        neutrino::failsafe
)

# The codec-axis flag macros are also surfaced as compile definitions so
# register_codecs.cc can #ifdef each registration call. Naming mirrors
# mzexplode's ONYX_ANIM_HAS_<CODEC> convention.
foreach(_codec FLC SMK BINK CDXL DPAN YAFA ATARI_SEQ AMIGA_ANIM)
    if (NEUTRINO_ONYX_ANIM_CODEC_${_codec})
        target_compile_definitions(onyx_anim_codecs PRIVATE
            ONYX_ANIM_HAS_${_codec}=1)
    endif ()
endforeach ()

if (NEUTRINO_ONYX_ANIM_CODEC_FLC)
    target_link_libraries(onyx_anim_codecs PRIVATE lib_flc)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_SMK)
    target_link_libraries(onyx_anim_codecs PRIVATE lib_smk)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_BINK)
    target_link_libraries(onyx_anim_codecs PRIVATE lib_bink)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_CDXL)
    target_link_libraries(onyx_anim_codecs PRIVATE lib_cdxl)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_DPAN)
    target_link_libraries(onyx_anim_codecs PRIVATE lib_dpan)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_YAFA)
    target_link_libraries(onyx_anim_codecs PRIVATE lib_yafa)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_ATARI_SEQ)
    target_link_libraries(onyx_anim_codecs PRIVATE lib_atari_seq)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_AMIGA_ANIM)
    target_link_libraries(onyx_anim_codecs PRIVATE lib_amiga_anim)
endif ()

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

# register_codecs.cc is always compiled; it #ifdefs out the
# registration calls for disabled codecs. Per-codec facade .cc files
# (which forward decode() calls into the sub-lib) are compiled only
# when their codec is enabled.
target_sources(onyx_anim_codecs PRIVATE src/register_codecs.cc)

# `bytes` is shared bit-reading utility code used by multiple codec
# sub-libs; always built regardless of which codecs are enabled.
add_subdirectory(src/bytes)

if (NEUTRINO_ONYX_ANIM_CODEC_FLC)
    target_sources(onyx_anim_codecs PRIVATE src/flc.cc)
    add_subdirectory(src/flc)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_SMK)
    target_sources(onyx_anim_codecs PRIVATE src/smk.cc)
    add_subdirectory(src/smk)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_BINK)
    target_sources(onyx_anim_codecs PRIVATE src/bink.cc)
    add_subdirectory(src/bink)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_CDXL)
    target_sources(onyx_anim_codecs PRIVATE src/cdxl.cc)
    add_subdirectory(src/cdxl)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_DPAN)
    target_sources(onyx_anim_codecs PRIVATE src/dpan.cc)
    add_subdirectory(src/dpan)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_YAFA)
    target_sources(onyx_anim_codecs PRIVATE src/yafa.cc)
    add_subdirectory(src/yafa)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_ATARI_SEQ)
    target_sources(onyx_anim_codecs PRIVATE src/atari_seq.cc)
    add_subdirectory(src/atari_seq)
endif ()
if (NEUTRINO_ONYX_ANIM_CODEC_AMIGA_ANIM)
    target_sources(onyx_anim_codecs PRIVATE src/amiga_anim.cc)
    add_subdirectory(src/amiga_anim)
endif ()
