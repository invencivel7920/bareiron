#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/bareiron"
if [ ! -x "$BIN" ]; then
  echo "Building server..."
  mkdir -p "${ROOT}/build"
  (cd "$ROOT" && gcc -O2 -Wall -Wextra -Iinclude src/*.c -o "$BIN" -lm)
fi
pkill -x bareiron 2>/dev/null || true
sleep 0.2
"$BIN" >/tmp/bareiron_test.log 2>&1 &
SRV=$!
sleep 0.5
cleanup() { kill $SRV 2>/dev/null || true; }
trap cleanup EXIT
cd "${ROOT}/tests/integration"
python3 -u test_ws_protocol.py
python3 -u test_fuzz.py
echo "INTEGRATION OK"
