cmake_minimum_required(VERSION 3.20)

project(onyx_anim
        VERSION 0.1.0
        DESCRIPTION "Animation format library (FLC, SMK) for the Neutrino ecosystem"
        LANGUAGES CXX C
)

# ============================================================================
# Neutrino CMake Integration
# ============================================================================

include(FetchContent)
include(GenerateExportHeader)

if (NOT DEFINED NEUTRINO_CMAKE_DIR)
    FetchContent_Declare(neutrino_cmake
            GIT_REPOSITORY https://github.com/devbrain/neutrino-cmake.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            UPDATE_COMMAND ""
    )
    FetchContent_MakeAvailable(neutrino_cmake)
    set(NEUTRINO_CMAKE_DIR "${neutrino_cmake_SOURCE_DIR}/cmake" CACHE PATH "Neutrino CMake modules directory")
endif ()

list(APPEND CMAKE_MODULE_PATH "${NEUTRINO_CMAKE_DIR}")
include(NeutrinoInit)

# ============================================================================
# Options
# ============================================================================

neutrino_define_library_options(onyx_anim)

# Forward the single user-facing BUILD_SHARED flag to all sub-libraries.
# Mirrors musac's pattern: one knob, multiple sub-targets.
set(ONYX_ANIM_BUILD_SHARED ${NEUTRINO_ONYX_ANIM_BUILD_SHARED})

if (NEUTRINO_ONYX_ANIM_BUILD_SHARED)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)
endif ()

# ============================================================================
# Dependencies
# ============================================================================

include(${NEUTRINO_CMAKE_DIR}/deps/onyx_image.cmake)
neutrino_fetch_onyx_image()

include(${NEUTRINO_CMAKE_DIR}/deps/musac.cmake)
neutrino_fetch_musac()

include(${NEUTRINO_CMAKE_DIR}/deps/expected.cmake)
neutrino_fetch_expected()

# PocketFFT — header-only FFT library used by Bink Audio (RDFT + DCT
# transforms). Vendored via FetchContent so the 3.8k-line header doesn't
# clutter our tree but the build is still self-contained.
FetchContent_Declare(pocketfft
        GIT_REPOSITORY https://gitlab.mpcdf.mpg.de/mtr/pocketfft.git
        GIT_TAG        cpp
        GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(pocketfft)
add_library(lib_pocketfft INTERFACE)
target_include_directories(lib_pocketfft INTERFACE ${pocketfft_SOURCE_DIR})
target_compile_features(lib_pocketfft INTERFACE cxx_std_17)

option(NEUTRINO_ONYX_ANIM_BUILD_BENCHMARKS
       "Build decoder performance benchmarks" OFF)

# ============================================================================
# Sub-libraries
# ============================================================================

add_subdirectory(sdk)
add_subdirectory(codecs)
add_subdirectory(player)

# ============================================================================
# Compiler Warnings
# ============================================================================

neutrino_target_warnings(onyx_anim_sdk)
neutrino_target_warnings(onyx_anim_codecs)
neutrino_target_warnings(onyx_anim_player)
neutrino_target_sanitizers(onyx_anim_sdk)
neutrino_target_sanitizers(onyx_anim_codecs)
neutrino_target_sanitizers(onyx_anim_player)

# ============================================================================
# Tests
# ============================================================================

if (NEUTRINO_ONYX_ANIM_BUILD_TESTS)
    include(${NEUTRINO_CMAKE_DIR}/deps/doctest.cmake)
    neutrino_fetch_doctest()

    enable_testing()
    add_subdirectory(test)

    # ------------------------------------------------------------------------
    # FLC cross-check vs ffmpeg
    #
    # If ffmpeg is available and the data corpus is present, register one
    # ctest per file that decodes via flc_to_ppm and bit-compares against
    # ffmpeg's libavcodec/flicvideo.c output.
    # ------------------------------------------------------------------------
    find_program(FFMPEG_EXECUTABLE ffmpeg)
    set(_xcheck_data "${PROJECT_SOURCE_DIR}/data/fli-flc")
    if (FFMPEG_EXECUTABLE AND EXISTS "${_xcheck_data}")
        message(STATUS "FLC cross-check: ffmpeg=${FFMPEG_EXECUTABLE}, data=${_xcheck_data}")

        file(GLOB _xcheck_files
             "${_xcheck_data}/*.fli"
             "${_xcheck_data}/*.FLI"
             "${_xcheck_data}/*.flc"
             "${_xcheck_data}/*.FLC"
        )
        foreach (_f IN LISTS _xcheck_files)
            get_filename_component(_name "${_f}" NAME)
            add_test(
                NAME    flc_xcheck.${_name}
                COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/flc_cross_check.sh"
                        "${_f}" "$<TARGET_FILE:flc_to_ppm>" "${FFMPEG_EXECUTABLE}"
            )
        endforeach ()
    elseif (NOT FFMPEG_EXECUTABLE)
        message(STATUS "FLC cross-check: ffmpeg not found, skipping")
    endif ()

    # ------------------------------------------------------------------------
    # Atari SEQ cross-check vs Randelshofer's reference Java decoder
    #
    # Requires: java in PATH, seqconverter.jar in $ENV{HOME}/Downloads (or
    # explicit -DSEQCONVERTER_JAR=...), and a compiled SeqToPpm.class in
    # tools/seqref/. We compile that class here if java + javac are present.
    # ------------------------------------------------------------------------
    find_program(JAVA_EXECUTABLE  java)
    find_program(JAVAC_EXECUTABLE javac)
    set(_seq_data "${PROJECT_SOURCE_DIR}/data/atari-seq")

    if (NOT DEFINED SEQCONVERTER_JAR)
        set(SEQCONVERTER_JAR "$ENV{HOME}/Downloads/seqconverter.jar"
            CACHE FILEPATH "Path to Werner Randelshofer's seqconverter.jar")
    endif ()

    if (JAVA_EXECUTABLE AND JAVAC_EXECUTABLE
            AND EXISTS "${SEQCONVERTER_JAR}"
            AND EXISTS "${_seq_data}")
        message(STATUS "SEQ cross-check: java=${JAVA_EXECUTABLE} jar=${SEQCONVERTER_JAR}")

        # Build the small SeqToPpm reference driver.
        set(_seqref_src "${PROJECT_SOURCE_DIR}/tools/seqref/SeqToPpm.java")
        set(_seqref_cls "${CMAKE_CURRENT_BINARY_DIR}/seqref/SeqToPpm.class")
        add_custom_command(
            OUTPUT "${_seqref_cls}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/seqref"
            # -sourcepath "" stops javac from recompiling the bundled .java
            # files inside the jar (which reference sun.applet, removed in
            # modern JDKs); we use the jar's pre-compiled .class files instead.
            COMMAND ${JAVAC_EXECUTABLE}
                    -cp "${SEQCONVERTER_JAR}"
                    -sourcepath ""
                    -d "${CMAKE_CURRENT_BINARY_DIR}/seqref"
                    "${_seqref_src}"
            DEPENDS "${_seqref_src}"
            VERBATIM
        )
        add_custom_target(seqref_class DEPENDS "${_seqref_cls}")

        file(GLOB_RECURSE _seq_files "${_seq_data}/*.SEQ" "${_seq_data}/*.seq")
        foreach (_f IN LISTS _seq_files)
            file(RELATIVE_PATH _rel "${_seq_data}" "${_f}")
            string(REPLACE "/" "_" _rel "${_rel}")
            add_test(
                NAME    seq_xcheck.${_rel}
                COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/atari_seq_cross_check.sh"
                        "${_f}"
                        "$<TARGET_FILE:seq_to_ppm>"
                        "${SEQCONVERTER_JAR}"
                        "${CMAKE_CURRENT_BINARY_DIR}/seqref"
            )
        endforeach ()
    elseif (NOT EXISTS "${SEQCONVERTER_JAR}")
        message(STATUS "SEQ cross-check: seqconverter.jar not found, skipping")
    endif ()

    # ------------------------------------------------------------------------
    # Amiga ANIM cross-check vs ffmpeg's libavcodec/iff.c
    #
    # Covers ANIM ops 3, 5, 7 (short/long), 8 (short/long), and J across
    # 1/8-bpp indexed and HAM6/HAM8 color modes.
    # ------------------------------------------------------------------------
    set(_anim_data "${PROJECT_SOURCE_DIR}/data/amiga-anim")
    if (FFMPEG_EXECUTABLE AND EXISTS "${_anim_data}")
        # Hand-curated list of files we expect to bit-match ffmpeg today.
        set(_anim_supported
            anim3_1bpp.anim
            anim3_8bpp.anim
            anim3_BirdofPrey.anim
            anim3_ham6.anim
            anim3_ham8.anim
            anim3_zeus.anim
            anim5_1bpp.anim
            anim5_8bpp.anim
            anim5_ham6.anim
            anim5_ham8.anim
            anim7s_1bpp.anim
            anim7s_8bpp.anim
            anim7s_ham6.anim
            anim7s_ham8.anim
            anim7l_1bpp.anim
            anim7l_8bpp.anim
            anim7l_ham6.anim
            anim7l_ham8.anim
            anim8s_1bpp.anim
            anim8s_8bpp.anim
            anim8s_ham6.anim
            anim8s_ham8.anim
            anim8l_1bpp.anim
            anim8l_8bpp.anim
            anim8l_ham6.anim
            anim8l_ham8.anim
            animJ_5bpp.anim
            animJ_5bpp_2.anim
            animJ_ham6.anim
            "anim108_ElGato(maybe_anim_1).anim"
        )
        foreach (_name IN LISTS _anim_supported)
            set(_path "${_anim_data}/${_name}")
            if (EXISTS "${_path}")
                add_test(
                    NAME    anim_xcheck.${_name}
                    COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/amiga_anim_cross_check.sh"
                            "${_path}" "$<TARGET_FILE:anim_to_ppm>" "${FFMPEG_EXECUTABLE}"
                )
            endif ()
        endforeach ()
    endif ()

    # ------------------------------------------------------------------------
    # Deluxe Paint Animation cross-check vs ffmpeg's libavcodec/anm.c
    #
    # Only the "clean" samples (no empty records) bit-match frame-for-frame.
    # See scripts/dpan_cross_check.sh for the rationale.
    # ------------------------------------------------------------------------
    set(_dpan_data "${PROJECT_SOURCE_DIR}/data/deluxe")
    if (FFMPEG_EXECUTABLE AND EXISTS "${_dpan_data}")
        set(_dpan_supported
            SW.ANM
            Vrs.anm
            LIGHT3D.001
            TITLES4B.001
            abydos.lpf.anim
        )
        foreach (_name IN LISTS _dpan_supported)
            set(_path "${_dpan_data}/${_name}")
            if (EXISTS "${_path}")
                add_test(
                    NAME    dpan_xcheck.${_name}
                    COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/dpan_cross_check.sh"
                            "${_path}" "$<TARGET_FILE:anim_to_ppm>" "${FFMPEG_EXECUTABLE}"
                )
            endif ()
        endforeach ()
    endif ()

    # ------------------------------------------------------------------------
    # CDXL cross-check vs ffmpeg's libavcodec/cdxl.c
    #
    # Every BIT_PLANAR sample in data/cdxl/ should bit-match ffmpeg's output.
    # The one BIT_LINE file (optologo.cdxl) is intentionally excluded — we
    # don't decode that layout yet.
    # ------------------------------------------------------------------------
    set(_cdxl_data "${PROJECT_SOURCE_DIR}/data/cdxl")
    if (FFMPEG_EXECUTABLE AND EXISTS "${_cdxl_data}")
        set(_cdxl_supported
            callnow.xl
            Discovery.CDXL
            Film.xl
            Fruit.CDXL
            logo.xl
            Maku.XL
            Spot.CDXL
            sub.xl
            vista1.xl
        )
        foreach (_name IN LISTS _cdxl_supported)
            set(_path "${_cdxl_data}/${_name}")
            if (EXISTS "${_path}")
                add_test(
                    NAME    cdxl_xcheck.${_name}
                    COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/cdxl_cross_check.sh"
                            "${_path}" "$<TARGET_FILE:anim_to_ppm>" "${FFMPEG_EXECUTABLE}"
                )
            endif ()
        endforeach ()
    endif ()

    # ------------------------------------------------------------------------
    # Smacker cross-check vs ffmpeg's libavcodec/smacker.c
    #
    # Every file in data/smacker/ should bit-match ffmpeg's output frame
    # for frame. EARTH.SMK is truncated mid-stream — both decoders emit
    # whatever fits in the file, and the common prefix matches.
    # ------------------------------------------------------------------------
    set(_smk_data "${PROJECT_SOURCE_DIR}/data/smacker")
    if (FFMPEG_EXECUTABLE AND EXISTS "${_smk_data}")
        set(_smk_supported
            1.smk
            1_2.SMK
            20130507_audio-distortion.smk
            EARTH.SMK
            FMAN.SMK
            SPLASH.SMK
            WMAN.SMK
            example.smk
            intro.smk
            test.smk
            wetlogo.smk
        )
        foreach (_name IN LISTS _smk_supported)
            set(_path "${_smk_data}/${_name}")
            if (EXISTS "${_path}")
                add_test(
                    NAME    smk_xcheck.${_name}
                    COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/smk_cross_check.sh"
                            "${_path}" "$<TARGET_FILE:anim_to_ppm>" "${FFMPEG_EXECUTABLE}"
                )
            endif ()
        endforeach ()
    endif ()

    # ------------------------------------------------------------------------
    # Bink (Bink1 video) cross-check vs ffmpeg's libavcodec/bink.c
    #
    # The cross-check compares YUV planes (via PPM → ffmpeg's RGB→YUV
    # roundtrip) — see scripts/bink_cross_check.sh for the rationale.
    # Our YUV decoder is bit-exact with ffmpeg's. The PPM-level mismatch
    # we used to see on these files was entirely a YUV→RGB conversion
    # rounding difference, not a real codec bug.
    #
    # ATI-9700-Animusic-Movie-v1.0.bik (73 MB, 6120 frames) shows a tiny
    # accumulating residual drift in the roundtrip comparison (sub-1 %).
    # It's excluded from the strict list — needs direct YUV-vs-YUV
    # tooling to determine whether it's a real codec bug or just RGB-
    # roundtrip noise on a long clip.
    # ------------------------------------------------------------------------
    set(_bink_data "${PROJECT_SOURCE_DIR}/data/bink")
    if (FFMPEG_EXECUTABLE AND EXISTS "${_bink_data}")
        set(_bink_supported
            end_victory.bik
            example.bik
            intro.bik
            logo_lucas.bik
            OpenPt1.bik
            original.bik
            phar_intro.bik
        )
        foreach (_name IN LISTS _bink_supported)
            set(_path "${_bink_data}/${_name}")
            if (EXISTS "${_path}")
                add_test(
                    NAME    bink_xcheck.${_name}
                    COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/bink_cross_check.sh"
                            "${_path}" "$<TARGET_FILE:anim_to_ppm>" "${FFMPEG_EXECUTABLE}"
                )
            endif ()
        endforeach ()
    endif ()
endif ()

# ============================================================================
# Examples
# ============================================================================

if (NEUTRINO_ONYX_ANIM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

# ============================================================================
# Benchmarks
# ============================================================================

if (NEUTRINO_ONYX_ANIM_BUILD_BENCHMARKS)
    if (CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(WARNING
            "NEUTRINO_ONYX_ANIM_BUILD_BENCHMARKS is ON in a Debug build. "
            "Benchmark numbers are only meaningful with optimized code "
            "(Release, RelWithDebInfo, or MinSizeRel).")
    endif ()
    add_subdirectory(benchmarks)
endif ()

# ============================================================================
# Summary
# ============================================================================

neutrino_is_top_level(_is_top_level)
if (_is_top_level)
    neutrino_print_options(onyx_anim)
endif ()
