# ============================================================================
# lib_amiga_anim — Amiga IFF ANIM byte-decoder primitives
#
# OBJECT library, internal to onyx_anim_codecs. Uses libiff (via the existing
# neutrino::iff target that onyx_image already pulls in) for IFF/ILBM tree
# walking; provides the ANIM-specific bits (ANHD parsing, DLTA op-5 delta,
# planar→chunky conversion).
# ============================================================================

add_library(lib_amiga_anim OBJECT
        src/header.cc
        src/decoders.cc
)

target_compile_features(lib_amiga_anim PUBLIC cxx_std_20)

target_include_directories(lib_amiga_anim
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# PUBLIC because anim/types.hh re-exports onyx_anim::expected.
target_link_libraries(lib_amiga_anim PUBLIC onyx_anim_sdk neutrino::iff)

set_target_properties(lib_amiga_anim PROPERTIES POSITION_INDEPENDENT_CODE ON)

neutrino_target_warnings(lib_amiga_anim)
neutrino_target_sanitizers(lib_amiga_anim)
