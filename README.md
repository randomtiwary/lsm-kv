# lsm-kv — Educational LSM-Tree Key-Value Store (C++)

A small **embedded key-value storage engine** built around a Log-Structured Merge-tree (LSM), implemented from scratch in modern **C++17** with CMake.

This project prioritizes **clarity over performance**. Every component is intentionally simple so you can read, review, and modify it while learning how production engines (LevelDB, RocksDB, BadgerDB) are structured.

## Architecture

```
Put/Delete ──► WAL ──► MemTable (SkipList)
                           │ flush
                           ▼
                     SSTable L0 (overlapping)
                           │ compact
                           ▼
                     SSTable L1 (non-overlapping ranges)

Get ──► MemTable ──► Immutable MemTable ──► L0 SSTables ──► L1 SSTables
```

| Component | Role |
|-----------|------|
| **Status / Slice** | Error reporting and non-owning byte views |
| **SkipList** | Concurrent in-memory ordered map (`shared_mutex`) |
| **MemTable** | SkipList wrapper with sequence numbers and tombstones |
| **WAL** | Append-only write-ahead log for crash recovery |
| **Block / SSTable** | Sorted on-disk tables with restart points and an index |
| **Version / Manifest** | Tracks live SSTables across levels |
| **DB** | Public `Open` / `Put` / `Get` / `Delete` / `Close` API with background flush & compaction |

## Public API

```cpp
#include "lsmkv/db.hpp"

lsmkv::Options options;
options.create_if_missing = true;

lsmkv::DB* db = nullptr;
lsmkv::Status s = lsmkv::DB::Open(options, "/tmp/mydb", &db);
s = db->Put(lsmkv::WriteOptions(), "hello", "world");

std::string value;
s = db->Get(lsmkv::ReadOptions(), "hello", &value);
s = db->Delete(lsmkv::WriteOptions(), "hello");
delete db;
```

## Multithreading

- **Writes** are serialized by a single `std::mutex` (WAL + MemTable update are atomic w.r.t. other writers).
- **Reads** use `std::shared_mutex` on the memtables and immutable snapshots of version metadata, so `Get` scales across threads.
- A background thread performs MemTable flushes and L0→L1 compaction.

## Build & Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Run the example

Easiest path (configures CMake if needed, builds `lsmkv_example`, runs it):

```bash
./scripts/run_example.sh
```

Manual equivalent:

```bash
cmake -S . -B build -DLSMKV_BUILD_EXAMPLES=ON
cmake --build build --target lsmkv_example
./build/lsmkv_example
```

Requirements: CMake ≥ 3.16, a C++17 compiler (GCC/Clang).

## Project layout

```
include/lsmkv/   Public headers
src/             Implementation
tests/           Exhaustive unit + integration tests
examples/        Minimal CLI example
docs/DESIGN.md   Design notes and PR roadmap
```

## Implementation roadmap

Work is split into small, independently reviewable PRs (see [docs/DESIGN.md](docs/DESIGN.md)):

1. CMake scaffold + CI + test harness
2. Core types (`Status`, `Slice`, options)
3. Concurrent `SkipList`
4. `MemTable` (sequence numbers, tombstones)
5. Write-ahead log
6. Block + SSTable format
7. Version set / manifest
8. `DB` engine (Put/Get/Delete, flush, recovery)
9. Compaction + multithreaded integration tests

## License

MIT
