#!/bin/bash -eu
#
# ClusterFuzzLite / OSS-Fuzz build script for SoundFileTap.
#
# Compiles the wrapper TU and the harness directly with the environment's
# compiler and sanitizer flags ($CFLAGS) rather than going through the
# project's CMake (which pins its own sanitizer flags for local use), so the
# code under test carries the engine's instrumentation. WORKDIR is the
# project root. $LIB_FUZZING_ENGINE links via $CXX per OSS-Fuzz convention.

$CC $CFLAGS -std=c11 -Iinclude -Ithird_party -c src/soundfile.c -o soundfile.o
$CC $CFLAGS -std=c11 -Iinclude -c fuzz/fuzz_wav.c -o fuzz_wav.o
$CXX $CXXFLAGS fuzz_wav.o soundfile.o $LIB_FUZZING_ENGINE -o "$OUT/fuzz_wav"

# Ship the seed corpus next to the target. OSS-Fuzz / ClusterFuzzLite
# automatically load <target>_seed_corpus.zip before fuzzing.
zip -j "$OUT/fuzz_wav_seed_corpus.zip" fuzz/corpus/*
