#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/dessmetal-erb-benchmark.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

c++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic \
  "$script_dir/benchmark_erb.cpp" -o "$build_dir/benchmark_erb"
"$build_dir/benchmark_erb" "$@"
