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
    src/transport/TransportInterface.cpp
    src/transport/ByteAccumulator.cpp
    src/device/DeviceSession.cpp
    src/device/DeviceManager.cpp
    src/sim/RegisterMap.cpp
    src/sim/RequestHandler.cpp
    src/sim/SimulatorConfig.cpp
    src/sim/SlaveSimulator.cpp
)

# 从站模拟器直接用套接字，Windows 下需要链接 ws2_32；其它平台没有这个库。
link_flags=()
case "${OS:-$(uname -s)}" in
    *Windows*|*MINGW*|*MSYS*|*CYGWIN*) link_flags+=(-lws2_32) ;;
esac

"$cxx" -std=c++17 -Wall -Wextra -O1 -I include \
    -o build_portable/portable_tests \
    "${sources[@]}" tests/portable/portable_tests.cpp "${link_flags[@]}"

"$cxx" -std=c++17 -Wall -Wextra -O1 -I include \
    -o build_portable/device_tests \
    "${sources[@]}" tests/portable/device_tests.cpp "${link_flags[@]}"

"$cxx" -std=c++17 -Wall -Wextra -O1 -I include \
    -o build_portable/simulator_tests \
    "${sources[@]}" tests/portable/simulator_tests.cpp "${link_flags[@]}"

"$cxx" -std=c++17 -Wall -Wextra -O1 -I include \
    -o build_portable/gateway_cli \
    "${sources[@]}" src/gateway_cli.cpp "${link_flags[@]}"

"$cxx" -std=c++17 -Wall -Wextra -O1 -I include \
    -o build_portable/slave_simulator \
    "${sources[@]}" src/sim/slave_simulator_main.cpp "${link_flags[@]}"

./build_portable/portable_tests
./build_portable/device_tests
./build_portable/simulator_tests
