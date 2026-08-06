# Fuzz targets compile src/soundfile.c themselves so the code under test is
# instrumented with the same sanitizer/fuzzer flags as the harness — linking
# the plain library target would fuzz uninstrumented code.

set(sf_fuzz_sources fuzz_wav.c ${CMAKE_CURRENT_SOURCE_DIR}/../src/soundfile.c)

# Real libFuzzer target (Clang only).
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_executable(fuzz_wav ${sf_fuzz_sources})
    target_include_directories(fuzz_wav PRIVATE ../include ../third_party)
    target_compile_options(fuzz_wav PRIVATE -g -O1 -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_wav PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# Standalone replay/mutation driver: any compiler, no fuzzer runtime.
add_executable(fuzz_wav_standalone ${sf_fuzz_sources} standalone_main.c)
target_include_directories(fuzz_wav_standalone PRIVATE ../include ../third_party)
if(NOT MSVC)
    target_compile_options(fuzz_wav_standalone PRIVATE -g -O1 -fsanitize=address,undefined)
    target_link_options(fuzz_wav_standalone PRIVATE -fsanitize=address,undefined)
endif()
