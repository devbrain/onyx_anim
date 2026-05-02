# ============================================================================
# lib_flc — internal FLI/FLC byte-decoder primitives
#
# OBJECT library: compiled once, .o files consumed directly by both
# onyx_anim_codecs (via $<TARGET_OBJECTS:lib_flc> + transitive deps) and the
# unit-test executable, so internal helpers can be tested without going through
# the public anim_decoder API.
#
# Free of musac / onyx_image / onyx_anim_sdk dependencies — only tl::expected.
# ============================================================================

add_library(lib_flc OBJECT
        src/header.cc
        src/chunks.cc
        src/decoders.cc
)

target_compile_features(lib_flc PUBLIC cxx_std_20)

target_include_directories(lib_flc
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# PUBLIC because flc/types.hh re-exports onyx_anim::expected and bytes::*
# symbols into the flc:: namespace; consumers of lib_flc's headers need both
# include trees on their compile path.
target_link_libraries(lib_flc PUBLIC onyx_anim_sdk lib_bytes)

set_target_properties(lib_flc PROPERTIES POSITION_INDEPENDENT_CODE ON)

neutrino_target_warnings(lib_flc)
neutrino_target_sanitizers(lib_flc)
