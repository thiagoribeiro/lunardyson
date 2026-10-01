#!/usr/bin/env bash
# Builds a platform wheel of the Python binding with liblunardyson.so bundled.
#   ./scripts/build_wheel.sh [build-dir]   → dist/lunardyson-<version>-py3-none-linux_<arch>.whl
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
PKG="$ROOT/bindings/python/lunardyson"
PYTHON="${PYTHON:-python3}"

test -f "$BUILD/liblunardyson.so" || { echo "missing $BUILD/liblunardyson.so — build the library first" >&2; exit 1; }
cp "$BUILD/liblunardyson.so" "$PKG/"
trap 'rm -f "$PKG/liblunardyson.so"' EXIT

mkdir -p "$ROOT/dist"
"$PYTHON" -m pip wheel --no-deps --quiet -w "$ROOT/dist" "$ROOT/bindings/python"

# The wheel carries a native library: tag it for this platform instead of "any".
"$PYTHON" -m pip install --quiet --user wheel 2>/dev/null || "$PYTHON" -m pip install --quiet wheel
for w in "$ROOT"/dist/lunardyson-*-py3-none-any.whl; do
    "$PYTHON" -m wheel tags --remove --platform-tag "linux_$(uname -m)" "$w"
done
ls -1 "$ROOT"/dist/*.whl
