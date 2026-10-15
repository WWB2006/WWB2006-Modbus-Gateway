#!/usr/bin/env bash
# 不依赖 Qt 与 CMake：只要有 g++/clang++ 就能验证协议层、设备层、模拟器、
# 串口分帧、配置解析、存储层与指标。
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

cxx="${CXX:-g++}"
cc="${CC:-gcc}"
mkdir -p build_portable

# 完全不依赖 Qt 的核心源码。任何 C++17 编译器都能编。
sources=(
    src/json/JsonValue.cpp
    src/config/GatewayConfig.cpp
    src/protocol/Crc16.cpp
    src/protocol/ExceptionCode.cpp
    src/protocol/ModbusFrame.cpp
    src/protocol/ModbusCodec.cpp
    src/transport/TransportInterface.cpp
    src/transport/ByteAccumulator.cpp
    src/transport/TransportRegistry.cpp
    src/transport/NativeTcpTransport.cpp
    src/device/DeviceSession.cpp
    src/device/DeviceManager.cpp
    src/sim/RegisterMap.cpp
    src/sim/RequestHandler.cpp
    src/sim/SimulatorConfig.cpp
    src/sim/SlaveSimulator.cpp
    src/sim/FaultInjector.cpp
    src/storage/MemoryStorage.cpp
    src/storage/SqliteStorage.cpp
    src/storage/CsvExport.cpp
    src/metrics/MetricsCollector.cpp
)

# 从站模拟器与存储层都用到套接字/DLL，Windows 下需要链接 ws2_32。
link_flags=()
case "${OS:-$(uname -s)}" in
    *Windows*|*MINGW*|*MSYS*|*CYGWIN*) link_flags+=(-lws2_32) ;;
esac

# sqlite3 是 vendored 的单文件 amalgamation（third_party/sqlite/sqlite3.c）。
# 关键：它是 **C 代码**，必须用 C 编译器单独编成 .o 再链接 —— 直接丢给 g++
# 当 C++ 编会因 void* 隐式转换、redefinition 等一堆只在 C 里合法的写法而失败。
# 这样每套测试只编一次 sqlite，而不是每套都重编 9MB 的源码。
sqlite_obj=build_portable/sqlite3.o
if [ ! -f "$sqlite_obj" ] || [ third_party/sqlite/sqlite3.c -nt "$sqlite_obj" ]; then
    "$cc" -O1 -w -I third_party/sqlite \
        -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_ENABLE_FTS5 \
        -DSQLITE_ENABLE_JSON1 -DSQLITE_ENABLE_RTREE -DSQLITE_DEFAULT_MEMSTATUS=0 \
        -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
        -c third_party/sqlite/sqlite3.c -o "$sqlite_obj"
fi

build_target() {
    local name="$1"
    local main="$2"
    "$cxx" -std=c++17 -Wall -Wextra -O1 -I include -I third_party/sqlite \
        -o "build_portable/$name" \
        "${sources[@]}" "$sqlite_obj" "$main" \
        "${link_flags[@]}"
}

build_target portable_tests  tests/portable/portable_tests.cpp
build_target device_tests    tests/portable/device_tests.cpp
build_target simulator_tests tests/portable/simulator_tests.cpp
build_target serial_tests    tests/portable/serial_tests.cpp
build_target config_tests    tests/portable/config_tests.cpp
build_target storage_tests   tests/portable/storage_tests.cpp
build_target fault_tests     tests/portable/fault_tests.cpp
build_target recovery_tests  tests/portable/recovery_tests.cpp
build_target native_transport_tests tests/portable/native_transport_tests.cpp
build_target gateway_cli     src/gateway_cli.cpp
build_target slave_simulator src/sim/slave_simulator_main.cpp
# 阶段 11 的常驻网关：不依赖 Qt。端口able 构建里编一份，
# 是为了在没有 Qt 的机器上也能验证「配置 → 采集 → 落库」这条链路本身能否编过。
# 注意：它用 TransportFactory，而工厂的 TCP 分支依赖 Qt；
# 无 Qt 时工厂只保留「不可用」分支，仍能编过（见 TransportFactory.cpp 的宏判断）。
build_target gateway_daemon  src/gateway_daemon.cpp

# 在构建目录内运行：测试写的临时文件（test_slave_config*.json、*.db、*.csv）
# 会落在 build_portable/ 里，与 CMake + ctest 路径下落盘在 build-linux/ 一致，
# 两种情况都不会让仓库根目录出现游离文件。
# 注意不要改回在仓库根目录运行——那会让测试把临时文件写到仓库根目录。
#
# 第一个参数是仓库根目录：需要读仓库真实文件的用例（config_tests）靠它拼绝对路径，
# 不靠当前工作目录猜。ctest 那边由 add_test 传 ${CMAKE_SOURCE_DIR}，两边一致。
tests=(portable_tests device_tests simulator_tests serial_tests config_tests storage_tests fault_tests recovery_tests native_transport_tests)
for test in "${tests[@]}"; do
    ( cd build_portable && "./$test" "$root" )
done
