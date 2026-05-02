# ============================================================================
# lib_bytes — header-only span-based binary I/O helpers
#
# INTERFACE library, internal to onyx_anim_codecs. Used by lib_flc (and any
# future codec lib in this tree). NOT depended on by onyx_anim_sdk: byte-level
# parsing is a codec implementation concern, not part of the public ABI.
# ============================================================================

add_library(lib_bytes INTERFACE)

target_compile_features(lib_bytes INTERFACE cxx_std_20)

target_include_directories(lib_bytes
        INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
