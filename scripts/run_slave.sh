#!/usr/bin/env bash
PORT=${1:-5020}
UNIT=${2:-1}
PROFILE=$3

EXE="./slave_simulator"
if [ ! -f "$EXE" ]; then
    EXE="./build-linux/src/slave_simulator"
fi

ARGS=("-p" "$PORT" "-u" "$UNIT")
if [ -n "$PROFILE" ]; then
    ARGS+=("-f" "$PROFILE")
fi

echo "Launching Slave Simulator on port $PORT (Unit $UNIT)..."
exec "$EXE" "${ARGS[@]}"
