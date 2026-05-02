#!/usr/bin/env bash
# Decode an Atari ST .SEQ file with both onyx_anim's seq_to_ppm and Werner
# Randelshofer's reference Java decoder, then bit-compare every frame.
#
# Usage:
#   atari_seq_cross_check.sh <input.seq> <seq_to_ppm_path> <seqconverter_jar> <SeqToPpm_class_dir>
#
# Exit codes:
#   0   all frames matched
#   1   at least one frame differs
#   2   reference or our tool failed
#   3   bad arguments

set -euo pipefail

if [[ $# -lt 4 ]]; then
    echo "usage: $0 <input.seq> <seq_to_ppm> <seqconverter.jar> <SeqToPpm class dir>" >&2
    exit 3
fi

INPUT=$1
TOOL=$2
JAR=$3
CLASSDIR=$4

[[ -x "$TOOL" ]]      || { echo "tool not executable: $TOOL" >&2;          exit 3; }
[[ -f "$JAR" ]]       || { echo "jar not found: $JAR" >&2;                  exit 3; }
[[ -f "$CLASSDIR/SeqToPpm.class" ]] || { echo "SeqToPpm.class missing in $CLASSDIR" >&2; exit 3; }
[[ -f "$INPUT" ]]     || { echo "input not a file: $INPUT" >&2;             exit 3; }
command -v java >/dev/null 2>&1 || { echo "java not in PATH" >&2; exit 2; }

OURS=$(mktemp -d)
REF=$(mktemp -d)
trap 'rm -rf "$OURS" "$REF"' EXIT

if ! "$TOOL" "$INPUT" "$OURS" >/dev/null; then
    echo "[$(basename "$INPUT")] seq_to_ppm failed" >&2
    exit 2
fi
if ! java -cp "$JAR:$CLASSDIR" SeqToPpm "$INPUT" "$REF" >/dev/null 2>&1; then
    echo "[$(basename "$INPUT")] reference decoder failed" >&2
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
if [[ ${#diffs[@]} -eq 0 && "$ours_count" == "$ref_count" ]]; then
    echo "[$base] OK ($ours_count frames)"
    exit 0
fi
echo "[$base] FAIL"
echo "  frames: ours=$ours_count, java=$ref_count"
if [[ ${#diffs[@]} -gt 0 ]]; then
    echo "  ${#diffs[@]} of $common common frames differ"
    for d in "${diffs[@]:0:5}"; do
        diff_byte=$(cmp "$OURS/$d" "$REF/$d" 2>&1 | head -1 || true)
        echo "    $d: $diff_byte"
    done
fi
exit 1
