#!/usr/bin/env bash
# Build (if needed) and run reldb_sql_server locally.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ ! -f build/CMakeCache.txt ]]; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLSMKV_BUILD_SERVER=ON
fi
cmake --build build --target reldb_sql_server -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"

exec ./build/reldb_sql_server "$@"
