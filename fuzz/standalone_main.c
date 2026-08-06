/*
  SoundFileTap -- standalone driver for fuzz_wav.c.

  Lets the fuzz target be built and exercised WITHOUT the libFuzzer runtime
  (e.g. with gcc + AddressSanitizer). This is NOT a coverage-guided fuzzer: it
  replays any inputs given on the command line and then runs a bounded,
  deterministic random-mutation loop over them. Use real libFuzzer
  (clang -fsanitize=fuzzer) for actual fuzzing; this driver is for CI smoke
  tests, crash-repro replay, and environments without the fuzzer runtime.

  Build:
      gcc -std=c11 -g -O1 -Iinclude -Ithird_party -fsanitize=address,undefined \
          src/soundfile.c fuzz/fuzz_wav.c fuzz/standalone_main.c -o fuzz_wav_standalone
      ./fuzz_wav_standalone fuzz/corpus/<each seed file>
*/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static const int k_iterations = 50000;

// xorshift64: deterministic, seedable, no libc rand() state.
static uint64_t sf_rng_state = 0x05CADAB7C0FFEE01ULL;

static uint64_t sf_rng(void) {
    uint64_t x = sf_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    sf_rng_state = x;
    return x;
}

typedef struct {
    uint8_t* data;
    size_t   size;
} sf_blob;

static sf_blob read_file(const char* path) {
    sf_blob blob = {NULL, 0};
    FILE*   f    = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "warning: could not open %s\n", path);
        return blob;
    }
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0 && fseek(f, 0, SEEK_SET) == 0) {
            blob.data = (uint8_t*)malloc((size_t)size);
            if (blob.data != NULL) {
                blob.size = fread(blob.data, 1, (size_t)size, f);
            }
        }
    }
    fclose(f);
    return blob;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("no inputs given; pass corpus files to replay/mutate\n");
        return 0;
    }

    sf_blob* seeds      = (sf_blob*)calloc((size_t)argc, sizeof(sf_blob));
    size_t   seed_count = 0;
    if (seeds == NULL) {
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        sf_blob blob = read_file(argv[i]);
        if (blob.data != NULL) {
            seeds[seed_count++] = blob;
        }
    }
    if (seed_count == 0) {
        free(seeds);
        return 0;
    }

    // 1) Replay every input verbatim (corpus entries and crash repros).
    for (size_t i = 0; i < seed_count; ++i) {
        LLVMFuzzerTestOneInput(seeds[i].data, seeds[i].size);
    }

    // 2) Bounded deterministic random-mutation loop over the seeds.
    size_t max_size = 0;
    for (size_t i = 0; i < seed_count; ++i) {
        if (seeds[i].size > max_size) {
            max_size = seeds[i].size;
        }
    }
    uint8_t* buf = (uint8_t*)malloc(max_size > 0 ? max_size : 1);
    if (buf == NULL) {
        return 1;
    }
    for (int iter = 0; iter < k_iterations; ++iter) {
        const sf_blob* seed = &seeds[sf_rng() % seed_count];
        if (seed->size == 0) {
            continue;
        }
        size_t size = seed->size;
        for (size_t i = 0; i < size; ++i) {
            buf[i] = seed->data[i];
        }

        // a handful of random single-byte flips
        int flips = 1 + (int)(sf_rng() % 6);
        for (int k = 0; k < flips; ++k) {
            buf[sf_rng() % size] = (uint8_t)(sf_rng() & 0xFF);
        }

        // occasionally truncate to probe short/edge-length handling
        if ((sf_rng() & 7) == 0 && size > 4) {
            size = sf_rng() % size;
        }
        LLVMFuzzerTestOneInput(buf, size);
    }

    printf("standalone fuzz driver: replayed %zu seed(s), ran %d mutations -- no crash\n", seed_count, k_iterations);
    free(buf);
    for (size_t i = 0; i < seed_count; ++i) {
        free(seeds[i].data);
    }
    free(seeds);
    return 0;
}
