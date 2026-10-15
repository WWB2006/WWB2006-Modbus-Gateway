# build_windows.ps1 - One command build on Windows
param(
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host " Building Modbus Gateway on Windows ($BuildType)..." -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan

$cmakeBin = "C:\Users\Lenovo\AppData\Local\Microsoft\WinGet\Packages\Kitware.CMake_Microsoft.Winget.Source_8wekyb3d8bbwe\cmake-4.4.4-windows-x86_64\bin"
if (Test-Path $cmakeBin) {
    $env:PATH = "$cmakeBin;" + $env:PATH
}

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=$BuildType
cmake --build build

Write-Host "`n>>> Running CTest unit tests..." -ForegroundColor Green
ctest --test-dir build --output-on-failure

Copy-Item build/src/gateway_cli.exe . -Force -ErrorAction SilentlyContinue
Copy-Item build/src/slave_simulator.exe . -Force -ErrorAction SilentlyContinue

Write-Host "`n[SUCCESS] Windows build and unit tests completed successfully!" -ForegroundColor Green
