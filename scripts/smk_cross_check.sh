#!/usr/bin/env bash
# Decode a Smacker (RAD Game Tools) file with both onyx_anim's anim_to_ppm
# and ffmpeg's libavcodec/smacker.c, then bit-compare every frame.
#
# Usage:
#   smk_cross_check.sh <input.smk> <anim_to_ppm_path> [<ffmpeg_path>]
#
# Smacker notes — truncated files (e.g. EARTH.SMK in our corpus) are
# tolerated by both decoders: each emits as many frames as fit within the
# file. We do a `min(ours, ffmpeg)` common-prefix compare in that case.

set -euo pipefail
if [[ $# -lt 2 ]]; then
    echo "usage: $0 <input.smk> <anim_to_ppm> [<ffmpeg>]" >&2
    exit 3
fi

INPUT=$1
TOOL=$2
FFMPEG=${3:-ffmpeg}

[[ -x "$TOOL" ]]                         || { echo "tool not executable: $TOOL" >&2; exit 3; }
command -v "$FFMPEG" >/dev/null 2>&1     || { echo "ffmpeg not found: $FFMPEG" >&2; exit 2; }
[[ -f "$INPUT" ]]                        || { echo "input not a file: $INPUT" >&2; exit 3; }

OURS=$(mktemp -d)
REF=$(mktemp -d)
trap 'rm -rf "$OURS" "$REF"' EXIT

if ! "$TOOL" "$INPUT" "$OURS" >/dev/null 2>&1; then
    echo "[$(basename "$INPUT")] anim_to_ppm failed" >&2
    exit 2
fi
if ! "$FFMPEG" -hide_banner -loglevel error \
        -i "$INPUT" \
        -fps_mode passthrough -start_number 0 -pix_fmt rgb24 \
        "$REF/frame_%04d.ppm" 2>/dev/null
then
    echo "[$(basename "$INPUT")] ffmpeg failed" >&2
    exit 2
fi

ours_count=$(find "$OURS" -name 'frame_*.ppm' | wc -l)
ref_count=$(find  "$REF"  -name 'frame_*.ppm' | wc -l)
common=$(( ours_count < ref_count ? ours_count : ref_count ))

diffs=()
for ((i=0; i < common; i++)); do
    name=$(printf 'frame_%04d.ppm' "$i")
    if ! cmp -s "$OURS/$name" "$REF/$name"; then
        diffs+=("$name")
    fi
done

base=$(basename "$INPUT")
delta=$(( ours_count > ref_count ? ours_count - ref_count : ref_count - ours_count ))
if [[ ${#diffs[@]} -eq 0 && $delta -eq 0 ]]; then
    echo "[$base] OK ($common matched)"
    exit 0
fi
echo "[$base] FAIL"
echo "  frames: ours=$ours_count, ffmpeg=$ref_count"
if [[ ${#diffs[@]} -gt 0 ]]; then
    echo "  ${#diffs[@]} of $common common frames differ"
    for d in "${diffs[@]:0:5}"; do
        diff_byte=$(cmp "$OURS/$d" "$REF/$d" 2>&1 | head -1 || true)
        echo "    $d: $diff_byte"
    done
fi
exit 1
