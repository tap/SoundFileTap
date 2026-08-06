#!/usr/bin/env bash
# Fetch the vendored dr_libs header(s) at a given upstream commit and record the pin.
#
# Usage:  third_party/update_upstream.sh <commit-sha>
#
# Downloads each file in FILES from mackron/dr_libs at <commit-sha>, overwrites
# the vendored copy, writes UPSTREAM_COMMIT, and prints a diff summary. Headers
# are vendored UNMODIFIED — all hardening lives in src/soundfile.c, never here.
#
# Update procedure (see README.md): run this script -> build -> unit tests ->
# short fuzz run over fuzz/corpus -> commit the header + UPSTREAM_COMMIT together.
set -euo pipefail

# Adding a decoder later (dr_flac.h, dr_mp3.h) is a one-line change here, plus
# the matching copy step in CMakeLists.txt/src.
FILES=(dr_wav.h)

if [ $# -ne 1 ]; then
    echo "usage: $0 <commit-sha>" >&2
    exit 2
fi

commit="$1"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for f in "${FILES[@]}"; do
    url="https://raw.githubusercontent.com/mackron/dr_libs/${commit}/${f}"
    tmp="$(mktemp)"
    echo "fetching ${url}"
    curl --fail --silent --show-error --location "$url" -o "$tmp"
    if [ -f "$here/$f" ]; then
        echo "--- diff summary for ${f} ---"
        diff --unified=0 "$here/$f" "$tmp" | diffstat 2>/dev/null || diff -q "$here/$f" "$tmp" || true
    fi
    mv "$tmp" "$here/$f"
done

echo "$commit" > "$here/UPSTREAM_COMMIT"
echo "pinned UPSTREAM_COMMIT = $commit"
echo "Now: build, run unit tests, run a short fuzz pass, then commit."
