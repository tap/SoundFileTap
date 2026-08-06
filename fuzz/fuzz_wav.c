/// @file fuzz_wav.c
/// @brief libFuzzer harness: arbitrary bytes -> tap_sf_open_memory -> stream to
/// exhaustion -> seek -> close. Exercises the whole untrusted-input surface the
/// way a consumer would. Built with ASan/UBSan by ci.yml (smoke) and
/// ClusterFuzzLite (PR + batch campaigns).

#include <stddef.h>
#include <stdint.h>

#include "tap/soundfile.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    tap_sf_config cfg;
    tap_sf_config_init(&cfg);
    cfg.max_total_bytes = (uint64_t)64 * 1024 * 1024; // keep fuzzing memory bounded

    tap_sf* sf = NULL;
    if (tap_sf_open_memory(&sf, data, size, &cfg) != tap_sf_ok) {
        return 0;
    }

    float    buf[4096];
    uint32_t channels = tap_sf_channels(sf);
    if (channels > 0) {
        uint64_t frames_per_read = (sizeof(buf) / sizeof(float)) / channels;
        if (frames_per_read == 0) {
            frames_per_read = 1; // unreachable with default limits; belt and braces
        }
        while (tap_sf_read_f32(sf, buf, frames_per_read) == frames_per_read) {
        }
        (void)tap_sf_seek(sf, tap_sf_frame_count(sf) / 2);
        (void)tap_sf_read_f32(sf, buf, frames_per_read);
    }
    tap_sf_close(sf);
    return 0;
}
