#!/usr/bin/env bash
set -Eeuo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/dessmetal-wav-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
sanitizers="${DESSMETAL_WAV_SANITIZERS:-undefined}"

case "$sanitizers" in
  undefined|address,undefined) ;;
  *) echo "Unsupported sanitizer set: $sanitizers" >&2; exit 2 ;;
esac

"${CXX:-clang++}" \
  -std=c++17 \
  -Wall -Wextra -Werror \
  -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG \
  -fsanitize="$sanitizers" \
  -fno-omit-frame-pointer \
  "$root_dir/AudioDSPTools/tests/test_wav.cpp" \
  "$root_dir/AudioDSPTools/dsp/wav.cpp" \
  -o "$build_dir/test_wav"

ir_files=()
while IFS= read -r -d '' ir_file; do
  ir_files+=("$ir_file")
done < <(find "$root_dir/NeuralAmpModeler/resources/models/IRs" -type f -iname '*.wav' \
  ! -name 'ENGL-Mix-5153-Rhythm.wav' -print0)
[[ "${#ir_files[@]}" == "16" ]]

UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1}" "$build_dir/test_wav" "${ir_files[@]}"
