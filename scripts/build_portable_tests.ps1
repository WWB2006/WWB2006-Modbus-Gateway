# 不依赖 Qt 与 CMake：只要有 g++ 就能验证协议层、设备层、模拟器与串口分帧（Windows）。
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$cxx = if ($env:CXX) { $env:CXX } else { "g++" }
New-Item -ItemType Directory -Force -Path "build_portable" | Out-Null

# 与 build_portable_tests.sh 保持一致：协议层、链路层、设备层、模拟器。
$sources = @(
    "src/json/JsonValue.cpp",
    "src/config/GatewayConfig.cpp",
    "src/protocol/Crc16.cpp",
    "src/protocol/ExceptionCode.cpp",
    "src/protocol/ModbusFrame.cpp",
    "src/protocol/ModbusCodec.cpp",
    "src/transport/TransportInterface.cpp",
    "src/transport/ByteAccumulator.cpp",
    "src/transport/TransportRegistry.cpp",
    "src/transport/NativeTcpTransport.cpp",
    "src/device/DeviceSession.cpp",
    "src/device/DeviceManager.cpp",
    "src/sim/RegisterMap.cpp",
    "src/sim/RequestHandler.cpp",
    "src/sim/SimulatorConfig.cpp",
    "src/sim/SlaveSimulator.cpp",
    "src/sim/FaultInjector.cpp",
    "src/storage/MemoryStorage.cpp",
    "src/storage/SqliteStorage.cpp",
    "src/storage/CsvExport.cpp",
    "src/metrics/MetricsCollector.cpp"
)

# sqlite3 是 vendored 的单文件 amalgamation，是 C 代码，必须用 C 编译器单独编成 .o。
$sqliteObj = "build_portable/sqlite3.o"
if (-not (Test-Path $sqliteObj) -or (Get-Item "third_party/sqlite/sqlite3.c").LastWriteTime -gt (Get-Item $sqliteObj).LastWriteTime) {
    & gcc -O1 -w -I third_party/sqlite -c third_party/sqlite/sqlite3.c -o $sqliteObj
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# 从站模拟器直接用套接字，Windows 下需要链接 ws2_32。
$linkFlags = @("-lws2_32")

$targets = @(
    @{ name = "portable_tests";  main = "tests/portable/portable_tests.cpp" },
    @{ name = "device_tests";    main = "tests/portable/device_tests.cpp" },
    @{ name = "simulator_tests"; main = "tests/portable/simulator_tests.cpp" },
    @{ name = "serial_tests";    main = "tests/portable/serial_tests.cpp" },
    @{ name = "config_tests";    main = "tests/portable/config_tests.cpp" },
    @{ name = "storage_tests";   main = "tests/portable/storage_tests.cpp" },
    @{ name = "fault_tests";     main = "tests/portable/fault_tests.cpp" },
    @{ name = "recovery_tests";  main = "tests/portable/recovery_tests.cpp" },
    @{ name = "native_transport_tests"; main = "tests/portable/native_transport_tests.cpp" },
    @{ name = "gateway_cli";     main = "src/gateway_cli.cpp" },
    @{ name = "slave_simulator"; main = "src/sim/slave_simulator_main.cpp" }
)

foreach ($target in $targets) {
    & $cxx -std=c++17 -Wall -Wextra -O1 -I include -I third_party/sqlite `
        -o "build_portable/$($target.name).exe" `
        @sources $sqliteObj $target.main @linkFlags
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# 在构建目录内运行：测试写的临时文件落在 build_portable/ 里，与 bash 脚本一致。
# 第一个参数是仓库根目录，与 ctest 的 ${CMAKE_SOURCE_DIR} 保持一致。
$tests = @("portable_tests", "device_tests", "simulator_tests", "serial_tests", "config_tests",
           "storage_tests", "fault_tests", "recovery_tests", "native_transport_tests")
foreach ($test in $tests) {
    Push-Location build_portable
    & "./$test.exe" $root
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { exit $code }
}
