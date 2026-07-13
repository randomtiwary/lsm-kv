#!/usr/bin/env bash
# Build (if needed) and run reldb_sql_cli against a running reldb_sql_server.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ ! -f build/CMakeCache.txt ]]; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
fi
cmake --build build --target reldb_sql_cli -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"

exec ./build/reldb_sql_cli "$@"
