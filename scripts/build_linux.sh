#!/usr/bin/env bash
# Ubuntu 构建脚本（需要 qt6-base-dev、cmake、g++）。
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

build_dir="${1:-build-linux}"

cmake -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" -j"$(nproc)"
ctest --test-dir "$build_dir" --output-on-failure
