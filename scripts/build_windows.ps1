# Windows 构建脚本（需要先装好 Qt 6 与 CMake）。
param(
    [string]$QtPrefix = "C:/Qt/6.5.3/mingw_64",
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

if (-not (Test-Path $QtPrefix)) {
    Write-Host "找不到 Qt：$QtPrefix" -ForegroundColor Yellow
    Write-Host "先用 -QtPrefix 指定实际安装路径，例如 C:/Qt/6.8.0/mingw_64" -ForegroundColor Yellow
}

cmake -B $BuildDir -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_PREFIX_PATH="$QtPrefix"
cmake --build $BuildDir -j

ctest --test-dir $BuildDir --output-on-failure
