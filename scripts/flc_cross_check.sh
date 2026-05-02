#!/usr/bin/env bash
# Decode an FLC/FLI file with both onyx_anim and ffmpeg, then bit-compare every
# frame.
#
# Usage:
#   flc_cross_check.sh <input.flc> <flc_to_ppm_path> [<ffmpeg_path>]
#
# Exit codes:
#   0   all frames matched
#   1   at least one frame differs
#   2   ffmpeg or our tool failed
#   3   bad arguments
#
# Frame counts can differ (ffmpeg may include the FLC ring frame as an extra
# trailing frame); we only compare frames that exist on both sides and report
# any count mismatch.

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <input.flc> <flc_to_ppm> [<ffmpeg>]" >&2
    exit 3
fi

INPUT=$1
TOOL=$2
FFMPEG=${3:-ffmpeg}

if [[ ! -x "$TOOL" ]]; then
    echo "flc_to_ppm not executable: $TOOL" >&2
    exit 3
fi
if ! command -v "$FFMPEG" >/dev/null 2>&1; then
    echo "ffmpeg not found: $FFMPEG" >&2
    exit 2
fi
if [[ ! -f "$INPUT" ]]; then
    echo "input not a file: $INPUT" >&2
    exit 3
fi

OURS=$(mktemp -d)
REF=$(mktemp -d)
trap 'rm -rf "$OURS" "$REF"' EXIT

# ----- Decode with onyx_anim -------------------------------------------------
if ! "$TOOL" "$INPUT" "$OURS" >/dev/null; then
    echo "[$(basename "$INPUT")] flc_to_ppm failed" >&2
    exit 2
fi

# ----- Decode with ffmpeg ----------------------------------------------------
# -vsync 0 / -fps_mode passthrough: emit exactly as many frames as the source.
# -start_number 0:                  match our 0-based naming.
# -pix_fmt rgb24:                   force unambiguous RGB output (PPM body).
if ! "$FFMPEG" -hide_banner -loglevel error \
        -i "$INPUT" \
        -fps_mode passthrough -start_number 0 -pix_fmt rgb24 \
        "$REF/frame_%04d.ppm" 2>/dev/null
then
    # Older ffmpeg uses -vsync 0 instead of -fps_mode.
    if ! "$FFMPEG" -hide_banner -loglevel error \
            -i "$INPUT" \
            -vsync 0 -start_number 0 -pix_fmt rgb24 \
            "$REF/frame_%04d.ppm"
    then
        echo "[$(basename "$INPUT")] ffmpeg failed" >&2
        exit 2
    fi
fi

# ----- Compare ---------------------------------------------------------------
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

# Pass criterion: every frame both decoders produced must match. Count
# mismatch of exactly 1 is the FLC ring frame (ffmpeg includes it; spec says
# frame_count excludes it). Anything else is a real problem.
if [[ ${#diffs[@]} -eq 0 ]]; then
    if [[ $delta -le 1 ]]; then
        if [[ "$ours_count" == "$ref_count" ]]; then
            echo "[$base] OK ($ours_count frames)"
        else
            echo "[$base] OK ($common frames matched; ffmpeg also emitted the ring frame)"
        fi
        exit 0
    fi
    echo "[$base] FAIL: frame count mismatch (ours=$ours_count, ffmpeg=$ref_count)"
    exit 1
fi

echo "[$base] FAIL"
echo "  frames: ours=$ours_count, ffmpeg=$ref_count"
echo "  ${#diffs[@]} of $common common frames differ"
for d in "${diffs[@]:0:5}"; do
    diff_byte=$(cmp "$OURS/$d" "$REF/$d" 2>&1 | head -1 || true)
    echo "    $d: $diff_byte"
done
exit 1
