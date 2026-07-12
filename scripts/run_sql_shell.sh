#!/usr/bin/env bash
# Build (if needed) and run examples/reldb_sql_shell.cpp.
#
# Usage (from repo root):
#   ./scripts/run_sql_shell.sh
#   ./scripts/run_sql_shell.sh --db /tmp/mydb
#   ./scripts/run_sql_shell.sh --rebuild
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
REBUILD=0
EXTRA=()
for arg in "$@"; do
  case "$arg" in
    --rebuild|-f) REBUILD=1 ;;
    -h|--help)
      sed -n '2,12p' "$0"
      exit 0
      ;;
    *)
      EXTRA+=("$arg")
      ;;
  esac
done

EXAMPLE_BIN="$BUILD_DIR/reldb_sql_shell"

need_configure=0
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  need_configure=1
elif [[ "$REBUILD" -eq 1 ]]; then
  need_configure=1
fi

if [[ "$need_configure" -eq 1 ]]; then
  cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DLSMKV_BUILD_EXAMPLES=ON \
    -DLSMKV_BUILD_TESTS=OFF
fi

cmake --build "$BUILD_DIR" --target reldb_sql_shell --parallel

if [[ ! -x "$EXAMPLE_BIN" ]]; then
  echo "error: expected executable at $EXAMPLE_BIN" >&2
  exit 1
fi

echo "==> running $EXAMPLE_BIN ${EXTRA[*]:-}"
exec "$EXAMPLE_BIN" "${EXTRA[@]}"
