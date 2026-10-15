#!/usr/bin/env bash
# package_linux.sh - Package headless gateway and slave simulator for Linux deployment
set -e

PKG_NAME="modbus-gateway-linux-x86_64"
DIST_DIR="dist/${PKG_NAME}"

echo "Creating Linux deployment package in ${DIST_DIR}..."
rm -rf dist
mkdir -p "${DIST_DIR}" "${DIST_DIR}/bin" "${DIST_DIR}/config" "${DIST_DIR}/scripts" "${DIST_DIR}/docs"

cp build-linux/src/gateway_cli "${DIST_DIR}/bin/" 2>/dev/null || cp gateway_cli "${DIST_DIR}/bin/"
cp build-linux/src/slave_simulator "${DIST_DIR}/bin/" 2>/dev/null || cp slave_simulator "${DIST_DIR}/bin/"

cp -r config/* "${DIST_DIR}/config/"
cp scripts/gateway.service "${DIST_DIR}/scripts/"
cp README.md "${DIST_DIR}/"
cp docs/* "${DIST_DIR}/docs/"

cd dist
tar -czvf "${PKG_NAME}.tar.gz" "${PKG_NAME}"
echo "[SUCCESS] Generated package: dist/${PKG_NAME}.tar.gz"
