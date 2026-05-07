FetchContent_Declare(nanobench
        GIT_REPOSITORY https://github.com/martinus/nanobench.git
        GIT_TAG        v4.3.11
        GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nanobench)

add_executable(bench_decode bench_decode.cc)
target_include_directories(bench_decode PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../codecs/src/bytes/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../codecs/src/flc/include
)
target_link_libraries(bench_decode PRIVATE
        neutrino::onyx_anim_sdk
        neutrino::onyx_anim_codecs
        neutrino::onyx_image
        nanobench::nanobench
)
target_compile_features(bench_decode PRIVATE cxx_std_20)
