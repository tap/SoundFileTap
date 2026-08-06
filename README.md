# SoundFileTap

[![CI](https://github.com/tap/SoundFileTap/actions/workflows/ci.yml/badge.svg)](https://github.com/tap/SoundFileTap/actions/workflows/ci.yml)
[![Tap House Style](https://github.com/tap/SoundFileTap/actions/workflows/style.yml/badge.svg)](https://github.com/tap/SoundFileTap/actions/workflows/style.yml)
[![ClusterFuzzLite](https://github.com/tap/SoundFileTap/actions/workflows/cflite_batch.yml/badge.svg)](https://github.com/tap/SoundFileTap/actions/workflows/cflite_batch.yml)

A small, **hardened sound file reading layer** for the Tap family, wrapping the
vendored, unmodified [dr_libs](https://github.com/mackron/dr_libs) WAV decoder
behind a single validated API. Plain C11 core (desktop and embedded), optional
C++ RAII layer on top.

Consumers include **only** `tap/soundfile.h` — never `dr_wav.h` directly. One
place to patch; every project inherits it.

## Why this exists

dr_libs is excellent and widely used, but it has had real security findings
(CVE-2025-14369, an integer-overflow allocation bomb in dr_flac; a heap
out-of-bounds write in dr_wav), and fixes land as unannounced commits — there
are no releases and no advisory feed. This wrapper adds the layers a hostile
input deserves:

- **Pre-validation before decode.** Our own bounded RIFF walk checks every
  chunk size against the physical file, the `fmt ` fields against configurable
  limits (channels, sample rate, frame count), and structural consistency
  (block align, bit depth, chunk ordering) — before dr_wav parses a byte.
- **Overflow-checked size arithmetic.** No unchecked
  `frames * channels * bytes` anywhere in our code.
- **A cumulative allocation cap.** Every allocation — including dr_wav's own —
  routes through accounting hooks that enforce `max_total_bytes` as a hard
  backstop, independent of pre-validation.
- **Streaming-first API.** There is deliberately no "decode whole file into one
  buffer" call; that is where allocation bombs live. Open, read blocks, close.
- **Reject, never clamp.** An out-of-bounds file fails with
  `tap_sf_err_bounds` and no partial state.
- **Cross-checked parses.** After dr_wav's own parse, its view of the file is
  compared against ours; disagreement rejects the file.

## Quick start

```cmake
include(FetchContent)
FetchContent_Declare(soundfiletap GIT_REPOSITORY https://github.com/tap/SoundFileTap GIT_TAG <tag>)
FetchContent_MakeAvailable(soundfiletap)
target_link_libraries(your_target PRIVATE tap::soundfile)
```

```c
#include "tap/soundfile.h"

tap_sf* sf = NULL;
if (tap_sf_open_file(&sf, "input.wav", NULL) != tap_sf_ok) { /* handle */ }

float buffer[1024 * 8];
uint64_t frames_per_block = 1024;
uint64_t got;
while ((got = tap_sf_read_f32(sf, buffer, frames_per_block)) > 0) {
    /* got frames of tap_sf_channels(sf)-interleaved f32 in [-1, 1) */
}
tap_sf_close(sf);
```

Limits and allocator hooks live in `tap_sf_config` (see `tap/soundfile.h`);
pass `NULL` for the defaults (64 channels, 384 kHz, 2 GiB cumulative
allocation). C++ users get an RAII `tap::soundfile::reader` in
`tap/soundfile.hpp`.

**Embedded builds:** configure with `-DTAP_SOUNDFILE_NO_STDIO=ON` for
memory/callback IO only (no `tap_sf_open_file`, no stdio dependency), and
provide allocator hooks — the defaults call libc `malloc`.

**Scope (v1):** WAV reading only — PCM 8/16/24/32-bit and float 32/64-bit,
including WAVE_FORMAT_EXTENSIBLE, detected by content sniffing. FLAC/MP3,
writing, and resampling (SampleRateTap's job) are out of scope until a
consumer needs them.

## Vendored upstream & update procedure

`third_party/dr_wav.h` is vendored **unmodified** at the commit recorded in
`third_party/UPSTREAM_COMMIT` (initial pin: `50bb723`, 2026-07-30, verified to
postdate the CVE-2025-14369 fix and the 2026 dr_wav malformed-file fixes; see
`third_party/README.md` for provenance and licensing). All hardening lives in
`src/soundfile.c`, so rebasing on upstream is trivial:

1. `third_party/update_upstream.sh <new-commit>`
2. Build + run unit tests (`cmake -B build -DTAP_SOUNDFILE_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build`)
3. Short fuzz pass (`fuzz/README.md`)
4. Commit the header and `UPSTREAM_COMMIT` together.

The weekly `upstream-check.yml` workflow maintains a tracking issue whenever
upstream HEAD moves past the pin — dr_libs ships security fixes silently, so
treat every new upstream commit list as potentially security-relevant.

## Testing & fuzzing

- Unit tests (GoogleTest, the family pin): round-trips across formats and
  channel counts, a malformed-file battery (size lies, truncations, ordering
  violations, codec mismatches), limit enforcement, and allocator-hook
  accounting. `ci.yml` runs them on Linux/macOS/Windows.
- Fuzzing: a libFuzzer harness over `tap_sf_open_memory` runs continuously via
  ClusterFuzzLite (per-PR code-change fuzzing + daily batch, ASan/UBSan), with
  a standalone driver for environments without the fuzzer runtime. See
  `fuzz/README.md`.

## Style

This repo follows the [Tap House Rules](STYLE.md), synced from
[TapHouse](https://github.com/tap/taphouse) and drift-checked in CI. Local
setup, once per clone: `pipx install pre-commit && pre-commit install`.

## License

MIT (Timothy Place). The vendored dr_wav header is public domain / MIT-0
dual-licensed; its license text is intact inside the header. See
`third_party/README.md`.
