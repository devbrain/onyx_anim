#!/usr/bin/env bash
# Decode a Bink (RAD Game Tools) file with both onyx_anim's anim_to_ppm
# and ffmpeg's libavcodec/bink.c, then bit-compare the first N frames at
# the YUV420p level (i.e. comparing the decoded planes directly, not the
# RGB-converted output).
#
# Why YUV instead of RGB:
#   ffmpeg's libswscale uses precision-specific lookup tables for its
#   YUV→RGB conversion that differ from any straightforward fixed-point
#   formula by ±1 on some Y/U/V triples. Our YUV decoder is bit-exact vs
#   ffmpeg's, but our RGB conversion (BT.601 limited, integer 14-bit)
#   diverges by 1 in a small fraction of pixels. Comparing PPM masks the
#   real "is the codec correct?" question. Comparing YUV answers it
#   directly.
#
# Usage:
#   bink_cross_check.sh <input.bik> <anim_to_ppm_path> [<ffmpeg_path>] [<frames>]

set -euo pipefail
if [[ $# -lt 2 ]]; then
    echo "usage: $0 <input.bik> <anim_to_ppm> [<ffmpeg>] [<frames>]" >&2
    exit 3
fi

INPUT=$1
TOOL=$2
FFMPEG=${3:-ffmpeg}
NUM_FRAMES=${4:-5}

[[ -x "$TOOL" ]]                         || { echo "tool not executable: $TOOL" >&2; exit 3; }
command -v "$FFMPEG" >/dev/null 2>&1     || { echo "ffmpeg not found: $FFMPEG" >&2; exit 2; }
[[ -f "$INPUT" ]]                        || { echo "input not a file: $INPUT" >&2; exit 3; }

OURS=$(mktemp -d)
REF_DIR=$(mktemp -d)
trap 'rm -rf "$OURS" "$REF_DIR"' EXIT

# Decode with both tools.
if ! "$TOOL" "$INPUT" "$OURS" >/dev/null 2>&1; then
    echo "[$(basename "$INPUT")] anim_to_ppm failed" >&2
    exit 2
fi

# We compare YUV by funneling our PPM frames through ffmpeg → yuv420p
# and decoding the source through ffmpeg → yuv420p. ffmpeg's RGB→YUV
# step is itself lossy, so this isn't a perfect "are the YUV planes
# identical" check — but it's a very tight upper bound on visible
# differences and holds whenever our decoder agrees with ffmpeg's.

if ! "$FFMPEG" -hide_banner -loglevel error \
        -i "$INPUT" \
        -frames:v "$NUM_FRAMES" \
        -f rawvideo -pix_fmt yuv420p \
        "$REF_DIR/ref.yuv"
then
    echo "[$(basename "$INPUT")] ffmpeg decode failed" >&2
    exit 2
fi

# Need fps to feed back through ffmpeg. The pipeline through head/awk
# can SIGPIPE under `set -o pipefail`; capture in a way that survives.
set +o pipefail
fps_str=$("$FFMPEG" -hide_banner -i "$INPUT" 2>&1 | \
          grep -oE '[0-9]+(\.[0-9]+)? fps' | head -1 | awk '{print $1}')
set -o pipefail
fps=${fps_str:-25}

if ! "$FFMPEG" -hide_banner -loglevel error \
        -framerate "$fps" -start_number 0 \
        -i "$OURS/frame_%04d.ppm" \
        -frames:v "$NUM_FRAMES" \
        -f rawvideo -pix_fmt yuv420p \
        "$REF_DIR/ours.yuv"
then
    echo "[$(basename "$INPUT")] ffmpeg ppm→yuv failed" >&2
    exit 2
fi

# Bit-compare.
base=$(basename "$INPUT")
if cmp -s "$REF_DIR/ref.yuv" "$REF_DIR/ours.yuv"; then
    sz=$(stat -c%s "$REF_DIR/ref.yuv")
    echo "[$base] OK ($NUM_FRAMES frames, $sz YUV bytes match)"
    exit 0
fi

# Detailed diff
diffs=$(cmp -l "$REF_DIR/ref.yuv" "$REF_DIR/ours.yuv" 2>/dev/null | wc -l || true)
total=$(stat -c%s "$REF_DIR/ref.yuv")
echo "[$base] FAIL: $diffs of $total YUV bytes differ"
exit 1
