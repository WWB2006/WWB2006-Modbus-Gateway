#!/usr/bin/env bash
# 不依赖 Qt 与 CMake：只要有 g++/clang++ 就能验证协议层。
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

cxx="${CXX:-g++}"
mkdir -p build_portable

sources=(
    src/protocol/Crc16.cpp
    src/protocol/ExceptionCode.cpp
    src/protocol/ModbusFrame.cpp
    src/protocol/ModbusCodec.cpp
)

"$cxx" -std=c++17 -Wall -Wextra -O1 -I include \
    -o build_portable/portable_tests \
    "${sources[@]}" tests/portable/portable_tests.cpp

"$cxx" -std=c++17 -Wall -Wextra -O1 -I include \
    -o build_portable/gateway_cli \
    "${sources[@]}" src/gateway_cli.cpp

./build_portable/portable_tests
