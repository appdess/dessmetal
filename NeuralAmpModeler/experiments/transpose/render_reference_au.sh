#!/bin/sh
set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
  echo "usage: $0 input-audio output-wav semitones [section-mask]" >&2
  exit 64
fi

input_path=$1
output_path=$2
semitones=$3
section_mask=${4:-31}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
render_dir=$(mktemp -d "${TMPDIR:-/tmp}/dessmetal-reference-render.XXXXXX")
trap 'rm -rf "$render_dir"' EXIT HUP INT TERM

sample_rate=$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate \
  -of default=noprint_wrappers=1:nokey=1 "$input_path")
ffmpeg -v error -y -i "$input_path" -map 0:a:0 -ac 1 -f f32le "$render_dir/input.f32le"
clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic "$script_dir/render_reference_au.cpp" \
  -framework AudioToolbox -o "$render_dir/render_reference_au"
"$render_dir/render_reference_au" "$render_dir/input.f32le" "$render_dir/output.f32le" \
  "$sample_rate" "$semitones" "$section_mask"
ffmpeg -v error -y -f f32le -ar "$sample_rate" -ac 1 -i "$render_dir/output.f32le" \
  -c:a pcm_s24le "$output_path"
