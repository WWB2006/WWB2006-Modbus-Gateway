#!/usr/bin/env bash
# build_linux.sh - One command build on Ubuntu 22.04 / Debian Linux
set -e

echo "========================================================"
echo " Building Modbus Gateway on Linux (Ubuntu / Debian)..."
echo "========================================================"

BUILD_DIR="build-linux"
cmake -B ${BUILD_DIR} -DCMAKE_BUILD_TYPE=Release
cmake --build ${BUILD_DIR} -j$(nproc)

echo ""
echo ">>> Running CTest unit tests..."
ctest --test-dir ${BUILD_DIR} --output-on-failure

cp ${BUILD_DIR}/src/gateway_cli . 2>/dev/null || true
cp ${BUILD_DIR}/src/slave_simulator . 2>/dev/null || true

echo ""
echo "[SUCCESS] Linux build and unit tests completed successfully!"
