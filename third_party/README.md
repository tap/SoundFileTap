# Vendored third-party code

## dr_wav.h

- **Upstream:** [mackron/dr_libs](https://github.com/mackron/dr_libs)
- **Pinned commit:** see [`UPSTREAM_COMMIT`](UPSTREAM_COMMIT) (one line, the full SHA).
  The initial pin `50bb723e6a459dbb781e26cefee4fd9ca6714d6a` (2026-07-30, dr_wav
  v0.14.6-dev) postdates the CVE-2025-14369 fix (`b2197b2`, verified ancestor) and the
  2026 run of dr_wav malformed-file bounds fixes (`b022119`, `c629ca6`, `64ba7a8`,
  `0100187`, `04e40d6`, `2800824`, `34a89ff`, `a2d0748`).
- **License:** dual-licensed — public domain (Unlicense) or MIT-0, at your option. The
  full license text is intact at the end of the vendored header; this repo redistributes
  it under those terms alongside SoundFileTap's own MIT license.
- **Modifications: none, ever.** The header is byte-identical to upstream at the pinned
  commit. All hardening (pre-validation, overflow-checked arithmetic, allocation caps)
  lives in `src/soundfile.c` so rebasing onto upstream HEAD is trivial.

Update with [`update_upstream.sh`](update_upstream.sh); the weekly
`upstream-check.yml` workflow opens a tracking issue when upstream HEAD moves past the
pin (dr_libs has no releases or advisories — silent commits are the only signal).
