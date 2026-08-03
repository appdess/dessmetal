#!/bin/sh
set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 8 ]; then
  echo "usage: $0 input-audio output-wav semitones [bandwidth-scale [low-bandwidth-scale [transition-hz [dominance-exponent [engine]]]]]" >&2
  exit 64
fi

input_path=$1
output_path=$2
semitones=$3
bandwidth_scale=${4:-1.0}
low_bandwidth_scale=${5:-0.0}
transition_hz=${6:-300.0}
dominance_exponent=${7:-0}
engine=${8:-erb}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
render_dir=$(mktemp -d "${TMPDIR:-/tmp}/dessmetal-erb-render.XXXXXX")
trap 'rm -rf "$render_dir"' EXIT HUP INT TERM

sample_rate=$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate \
  -of default=noprint_wrappers=1:nokey=1 "$input_path")
ffmpeg -v error -y -i "$input_path" -map 0:a:0 -ac 1 -f f64le "$render_dir/input.f64le"
c++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic \
  "$script_dir/render_erb_file.cpp" -o "$render_dir/render_erb_file"
"$render_dir/render_erb_file" "$render_dir/input.f64le" "$render_dir/output.f64le" \
  "$sample_rate" "$semitones" "$bandwidth_scale" "$low_bandwidth_scale" "$transition_hz" \
  "$dominance_exponent" "$engine"
ffmpeg -v error -y -f f64le -ar "$sample_rate" -ac 1 -i "$render_dir/output.f64le" \
  -c:a pcm_s24le "$output_path"
