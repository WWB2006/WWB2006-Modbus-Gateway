# 不依赖 Qt 与 CMake：只要有 g++ 就能验证协议层（Windows）。
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$cxx = if ($env:CXX) { $env:CXX } else { "g++" }
New-Item -ItemType Directory -Force -Path "build_portable" | Out-Null

$sources = @(
    "src/protocol/Crc16.cpp",
    "src/protocol/ExceptionCode.cpp",
    "src/protocol/ModbusFrame.cpp",
    "src/protocol/ModbusCodec.cpp",
    "src/transport/TransportInterface.cpp",
    "src/transport/ByteAccumulator.cpp",
    "src/device/DeviceSession.cpp",
    "src/device/DeviceManager.cpp"
)

& $cxx -std=c++17 -Wall -Wextra -O1 -I include `
    -o build_portable/portable_tests.exe `
    @sources tests/portable/portable_tests.cpp
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cxx -std=c++17 -Wall -Wextra -O1 -I include `
    -o build_portable/device_tests.exe `
    @sources tests/portable/device_tests.cpp
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cxx -std=c++17 -Wall -Wextra -O1 -I include `
    -o build_portable/gateway_cli.exe `
    @sources src/gateway_cli.cpp
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "./build_portable/portable_tests.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "./build_portable/device_tests.exe"
