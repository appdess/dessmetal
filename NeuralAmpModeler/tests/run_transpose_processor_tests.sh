#!/usr/bin/env bash
set -Eeuo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source_file="$repo_root/NeuralAmpModeler/tests/test_transpose_processor.cpp"
build_dir="${TMPDIR:-/tmp}/dessmetal-transpose-processor-test"
binary="$build_dir/test_transpose_processor"

mkdir -p "$build_dir"
clang++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Werror "$source_file" -o "$binary"
"$binary"
