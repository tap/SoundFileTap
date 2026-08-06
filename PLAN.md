# SoundFileTap — Planning Document

**Repo:** `github.com/tap/SoundFileTap`
**License:** MIT
**Purpose:** A small, hardened sound file reading layer for the Tap family, wrapping an
unmodified upstream [dr_libs](https://github.com/mackron/dr_libs) decoder behind a single
validated API.

> **Revision 2.** Supersedes the initial draft after review against the family repos.
> Settled decisions: **WAV-only v1** (FLAC deferred — no current consumer reads FLAC, or
> indeed any sound file; see §7), full alignment with the family conventions recorded in
> TapHouse (`tap/soundfile.h` header path, `TAP_SOUNDFILE_*` option/macro prefix),
> **ClusterFuzzLite** for fuzzing CI (OscTap's proven pattern, not a hand-rolled workflow),
> and **GoogleTest** for unit tests (the family standard, pinned like the siblings).

---

## 1. Motivation & Design Principles

- dr_libs has had recent security findings: **CVE-2025-14369** (integer overflow in
  dr_flac — `totalPCMFrameCount` trusted from STREAMINFO before buffer-size math; CERT
  VU#924114) and a heap out-of-bounds write in dr_wav. Fixes land as unannounced commits;
  there are no tagged releases or security advisories. The FLAC CVE lands in code we defer
  (§10), but it demonstrates the *pattern* this wrapper exists to guard against, and the
  dr_wav finding is squarely in scope.
- **Do not fork dr_libs.** Vendor the unmodified upstream header pinned to a specific
  commit. All hardening lives in our wrapper so we can rebase on upstream HEAD trivially.
- **One API surface.** Consuming projects include only `tap/soundfile.h` — never
  `dr_wav.h` directly. One place to patch; every project inherits it.
- **Validate before decode.** Pre-parse headers, enforce configurable sanity bounds,
  reject pathological files before dr_libs allocates anything.
- **Streaming-first.** No "decode whole file to one buffer" convenience in v1 — that is
  where allocation bombs live (it is exactly where CVE-2025-14369 lived). Callers read in
  blocks.
- Plain **C11** core (wrapper + decoder), so it compiles for desktop (Max/MSP externals,
  macOS/Windows/Linux) and embedded targets (ARM Cortex-M55, Hexagon). Optional C++
  convenience header on top.

## 2. Family Conventions (TapHouse)

This repo is born convention-correct — it follows the targets recorded in TapHouse's
README rather than the legacy forms older repos still carry:

| Knob | Value |
|------|-------|
| Namespace (C++ layer) | `tap::soundfile` |
| CMake alias | `tap::soundfile` (no legacy second alias — nothing predates it) |
| Header path | `include/tap/soundfile.h`, included as `"tap/soundfile.h"` |
| CMake options / macros | `TAP_SOUNDFILE_*` (upper-snake of the namespace) |
| C symbol prefix | `tap_sf_` for functions/types — a deliberate short form for an API called in loops; macros stay `TAP_SOUNDFILE_*` |

TapHouse adoption is part of the initial scaffold, not a follow-up:

- Run `taphouse/scripts/sync.sh` and commit the four root configs
  (`.clang-format`, `.clang-tidy`, `STYLE.md`, `.pre-commit-config.yaml`).
- `style.yml` with the format gate plus the shared drift check, pinned to the current tag:
  `uses: tap/taphouse/.github/workflows/drift-check.yml@v5` / `ref: v5`.
- The synced `.claude/settings.json` + session-start hook and the family PR template.
- ~~Open item~~ **Resolved during scaffold:** the `.clang-tidy` naming rules fit C11
  as-is — public struct fields are bare `lower_case` (no `m_`; the prefix applies only to
  private/protected members, which C doesn't have), functions/types are snake_case, and
  enum constants are `lower_case` (hence `tap_sf_ok`, not `TAP_SF_OK` — matching
  `converter_state::acquire`-style family precedent). No STYLE.md exception needed; the
  single carve-out is a `NOLINT` on the POSIX feature-test macros (`_FILE_OFFSET_BITS`,
  `_POSIX_C_SOURCE`), whose names the standard fixes.

## 3. Repository Layout

```
SoundFileTap/
├── LICENSE                     # MIT (Timothy Place)
├── README.md
├── PLAN.md                     # this document
├── CMakeLists.txt              # library + tests + fuzz targets; FetchContent-friendly
├── .clang-format ...           # synced from TapHouse (see §2)
├── include/
│   └── tap/
│       ├── soundfile.h         # the public API (single header, C11)
│       └── soundfile.hpp       # optional C++ convenience layer (tap::soundfile)
├── src/
│   └── soundfile.c             # implementation; the ONLY TU that includes dr_wav.h
├── third_party/
│   ├── dr_wav.h                # vendored, unmodified
│   ├── README.md               # provenance + licensing (see §9)
│   ├── UPSTREAM_COMMIT         # pinned commit hash, one line
│   └── update_upstream.sh      # fetches header(s) at a given commit, updates UPSTREAM_COMMIT
├── tests/
│   ├── CMakeLists.txt          # GoogleTest via FetchContent, family pin (see §7)
│   ├── test_soundfile.cpp      # C++ TUs testing the C API (WAVs built in memory)
│   └── make_corpus.py          # generates the fuzz seed corpus, deterministically
├── fuzz/
│   ├── fuzz_wav.c              # libFuzzer entry: bytes → tap_sf_open_memory
│   ├── standalone_main.c       # driver for compilers without the libFuzzer runtime
│   └── corpus/                 # tiny (<10 KB) checked-in seeds; the one corpus location
├── .clusterfuzzlite/
│   ├── Dockerfile
│   ├── build.sh                # compiles harness with $CXX/$CFLAGS + $LIB_FUZZING_ENGINE,
│   │                           #   zips fuzz/corpus as fuzz_wav_seed_corpus.zip
│   └── project.yaml
└── .github/workflows/
    ├── ci.yml                  # build + unit tests, macOS/Linux/Windows; fuzz-smoke job
    ├── style.yml               # format gate + TapHouse drift check @v5
    ├── cflite_pr.yml           # ClusterFuzzLite: code-change fuzzing on PRs, ASan+UBSan
    ├── cflite_batch.yml        # ClusterFuzzLite: scheduled longer batch runs
    └── upstream-check.yml      # weekly: compare mackron/dr_libs HEAD vs UPSTREAM_COMMIT
```

## 4. Public API (`tap/soundfile.h`)

Keep it deliberately small. Sketch — refine naming/signatures during implementation but
preserve the shape:

```c
typedef struct tap_sf tap_sf;                    // opaque

typedef enum { TAP_SF_OK = 0, TAP_SF_ERR_IO, TAP_SF_ERR_FORMAT,
               TAP_SF_ERR_BOUNDS, TAP_SF_ERR_ALLOC, TAP_SF_ERR_ARG } tap_sf_result;

typedef struct {
    // validation limits:
    uint32_t max_channels;        // default 64  (ambisonics up to ~7th order + headroom)
    uint32_t max_sample_rate;     // default 384000
    uint64_t max_frames;          // default: derived from max_total_bytes
    uint64_t max_total_bytes;     // default 2 GiB decoded; hard cap on cumulative allocation
    // allocator hooks for embedded targets — note old_size parameters: the wrapper
    // enforces max_total_bytes cumulatively, and libc-style signatures can't say how
    // much a free/realloc releases. We define these signatures, so we pass it.
    void* (*malloc_fn) (size_t size, void* user);
    void* (*realloc_fn)(void* p, size_t old_size, size_t new_size, void* user);
    void  (*free_fn)   (void* p, size_t old_size, void* user);
    void* alloc_user;
} tap_sf_config;

void          tap_sf_config_init(tap_sf_config* out);   // fill with defaults
tap_sf_result tap_sf_open_file  (tap_sf** out, const char* utf8_path, const tap_sf_config* cfg);
tap_sf_result tap_sf_open_memory(tap_sf** out, const void* data, size_t bytes, const tap_sf_config* cfg);

uint32_t tap_sf_channels(const tap_sf*);
uint32_t tap_sf_sample_rate(const tap_sf*);
uint64_t tap_sf_frame_count(const tap_sf*);

// Streaming reads into caller-provided buffers (deinterleaved-friendly variants welcome):
uint64_t      tap_sf_read_f32(tap_sf*, float* interleaved, uint64_t frames);
tap_sf_result tap_sf_seek(tap_sf*, uint64_t frame);
void          tap_sf_close(tap_sf*);
```

Notes:
- `tap_sf_config` (not `_limits`): it carries limits *and* allocator wiring.
- dr_wav's own allocation callbacks do **not** pass old sizes, so the wrapper keeps
  per-allocation bookkeeping (a small size header or a tracking table) to bridge dr_wav's
  callbacks onto ours. State this in the implementation comments — it is load-bearing for
  the `max_total_bytes` backstop, not an optimization.
- Format detection by content sniffing (RIFF magic), not extension. Unknown magic →
  `TAP_SF_ERR_FORMAT` (leaves room for FLAC later without an API break).
- `tap_sf_open_memory` is the fuzzing entry point; `open_file` is a thin layer over
  dr_wav callbacks. `utf8_path` means UTF-8 **on Windows too** — convert and use the
  `_w` open path there; MSVC's `fopen` is ANSI and must not be the default behavior.
- The C++ header (`soundfile.hpp`, namespace `tap::soundfile`) is a thin RAII wrapper —
  no additional functionality.

## 5. Hardening Requirements (the actual point)

Before handing anything to dr_wav, the wrapper must:

1. **Pre-validate WAV:** parse the RIFF chunk list itself (bounded loop, max chunk count,
   chunk sizes checked against remaining file size); verify `fmt ` sanity (channels ≤
   limit, rate ≤ limit, bit depth ∈ {8,16,24,32,64 float}, block align consistent);
   verify `data` size against file size; handle WAVE_FORMAT_EXTENSIBLE.
2. **All size arithmetic in the wrapper is overflow-checked**
   (`__builtin_mul_overflow` or equivalent). No unchecked
   `frames * channels * bytes_per_sample` anywhere in our code. This is the
   CVE-2025-14369 pattern generalized, applied to WAV from day one.
3. **Route all dr_wav allocations through our allocator hooks** (dr_wav supports custom
   allocation callbacks) and enforce `max_total_bytes` cumulatively inside the hook — a
   hard backstop independent of pre-validation.
4. Reject rather than clamp: out-of-bounds files return `TAP_SF_ERR_BOUNDS` with no
   partial state.
5. Compile the vendored header with features we don't use disabled where upstream flags
   allow (e.g. `DR_WAV_NO_STDIO` on embedded builds).

## 6. Upstream Management

- `third_party/update_upstream.sh <commit>` downloads `dr_wav.h` from `mackron/dr_libs` at
  that commit (raw.githubusercontent.com), writes `UPSTREAM_COMMIT`, and prints a diff
  summary. (Written to take a file list, so adding `dr_flac.h` later is a one-line change.)
- Initial pin: **latest upstream HEAD at time of repo creation** — must postdate the
  dr_wav heap-OOB-write fix; verify against upstream history at scaffold time and record
  the commit + what it includes in README.
- `upstream-check.yml`: weekly scheduled Action queries the GitHub API for
  `mackron/dr_libs` HEAD; if it differs from `UPSTREAM_COMMIT`, open (or update) a
  tracking issue listing the new commits. This substitutes for the advisory feed dr_libs
  doesn't have.
- README documents the update procedure: run script → build → unit tests → short fuzz
  run → commit.

## 7. Testing

**Framework: GoogleTest** — the family standard (TapHouse's divergence log records
Catch2-in-TapTools as the one deliberate exception, not a precedent). Fetched exactly as
the siblings do:

```cmake
GIT_REPOSITORY https://github.com/google/googletest.git
GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571 # v1.14.0
```

Test TUs are C++ exercising the C API — no third framework, no hand-rolled test macros.

**Coverage** (`ci.yml`, all three OSes):
- Round-trip known-good files: mono/stereo/8-channel WAV at 16/24/32f bit depths;
  44.1k/48k/96k; a WAVE_FORMAT_EXTENSIBLE file; an ambiX-style 16-channel file.
- Negative tests: truncated files, chunk size lies (larger than file), zero channels,
  absurd channel counts, fmt/data ordering oddities, odd-sized chunks with/without pad
  bytes, non-RIFF magic.
- Config tests: verify each limit field of `tap_sf_config` actually rejects, and that
  the cumulative-allocation backstop trips even when pre-validation is (hypothetically)
  bypassed.
- `tests/make_corpus.py` generates corpus files; commit only tiny (<10 KB) seeds.

## 8. Fuzzing — ClusterFuzzLite (OscTap's pattern)

Reuse the family's proven setup rather than a bespoke workflow:

- `fuzz/fuzz_wav.c`: libFuzzer harness, bytes → `tap_sf_open_memory` → stream-read to
  exhaustion → seek → close. `fuzz/standalone_main.c` drives the same harness under any
  compiler when the libFuzzer runtime is unavailable (documented in `fuzz/README.md`,
  as in OscTap).
- `.clusterfuzzlite/build.sh` compiles the harness directly with the environment's
  `$CC/$CFLAGS` + `$LIB_FUZZING_ENGINE` (not through our CMake, which pins its own
  sanitizer flags for local use) and zips `fuzz/corpus` as `fuzz_wav_seed_corpus.zip`.
- `cflite_pr.yml`: coverage-guided, code-change-targeted fuzzing on every PR,
  ASan + UBSan matrix, ~120 fuzz-seconds — mirrors OscTap's file nearly verbatim.
- `cflite_batch.yml`: scheduled longer batch runs with corpus pruning.
- `ci.yml` additionally gets a quick fuzz-smoke job (standalone driver over the corpus)
  so plain CI catches gross regressions without the Docker-based CFLite machinery.
- Crash reproductions get added to `tests/` as regression tests **and** to the corpus.

## 9. Consumption & Licensing

- CMake: installable target `tap::soundfile`, FetchContent-friendly (no global state, no
  forced compiler flags leaking to parents).
- Embedded builds: CMake option `TAP_SOUNDFILE_NO_STDIO` → memory/callback IO only;
  allocator hooks mandatory in that configuration (defaults call libc malloc otherwise —
  document it).
- **Consumers: none yet — and that is a correction to the first draft**, which listed
  "AmbiTap and SampleRateTap replace direct dr_wav includes" as follow-ups. Verified
  against both repos: neither (nor OscTap) includes dr_libs or reads sound files at all
  today; AmbiTap's only WAV code is a ~40-line inline *writer* in one example (writing
  is out of scope here anyway). SoundFileTap is built ahead of need, so v1 scope stays
  minimal and consumer migration PRs happen when a repo actually grows a file-reading
  feature.
- This repo: **MIT**, copyright Timothy Place. Vendored dr_libs headers are dual-licensed
  public domain (Unlicense) / MIT-0 — compatible with redistribution. Keep upstream
  license text intact inside the vendored header; note provenance + pinned commit in
  README and `third_party/README.md`.

## 10. Deliverables Checklist

- [x] Repo scaffold per §3, MIT LICENSE, README with badges, purpose, quick-start,
      update procedure
- [x] TapHouse adoption per §2: sync configs, `style.yml` (format gate + drift check
      @v5), pre-commit wiring, session-start hook; C11/clang-tidy open item resolved
- [x] `tap/soundfile.h` / `src/soundfile.c` implementing §4 with all §5 hardening
- [x] `tap/soundfile.hpp` C++ convenience layer (`tap::soundfile`)
- [x] `update_upstream.sh` + initial vendored `dr_wav.h` pinned at `50bb723` (postdates
      the dr_wav heap-OOB and 2026 malformed-file fixes; CVE-2025-14369 fix verified an
      ancestor), `third_party/README.md` documenting provenance and licensing
- [x] GoogleTest unit tests + `make_corpus.py` per §7
- [x] `fuzz_wav.c` + `standalone_main.c` + `.clusterfuzzlite/` + both CFLite workflows
      per §8
- [x] `ci.yml` (3-OS build + tests + fuzz-smoke + no-stdio build) and `upstream-check.yml`

**Out of scope for v1:** FLAC (dr_flac is the larger attack surface and no consumer
reads FLAC — add it behind the same API when a project needs it, with STREAMINFO
pre-validation and overflow-checked size math guarding the CVE-2025-14369 pattern),
MP3 (same reasoning, dr_mp3), writing files, resampling (SampleRateTap's job), float64
output, tag/metadata reading beyond what's needed for validation.
