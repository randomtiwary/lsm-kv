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

## TCP server

A small line-oriented TCP front-end exposes `GET` / `SET` / `DEL` over the embedded engine (default port **7379**). Concurrent connections are capped (default **128**, override with `--max-clients`).

```bash
./scripts/run_server.sh --db /tmp/lsmkv_data --port 7379 --max-clients 128
# or: cmake --build build --target lsmkv_server && ./build/lsmkv_server --db /tmp/lsmkv_data
```

Protocol (one command per line; keys/values must not contain newlines):

| Request | Response |
|---------|----------|
| `PING` | `+PONG` |
| `SET <key> <value>` | `+OK` |
| `GET <key>` | `$N` then a line with `N` bytes of value, or `NOT_FOUND` if missing |
| `DEL <key>` | `+OK` |
| `QUIT` | `+OK` (then connection closes) |
| errors | `-ERR <message>` |

```bash
printf 'SET hello world\nGET hello\nQUIT\n' | nc -q 1 127.0.0.1 7379
```

### Docker

Run two independent instances (separate data volumes, ports 7379 and 7380):

```bash
docker compose up --build
printf 'SET hello from1\nGET hello\nQUIT\n' | nc -q 1 127.0.0.1 7379
printf 'SET hello from2\nGET hello\nQUIT\n' | nc -q 1 127.0.0.1 7380
```

Single container:

```bash
docker build -t lsmkv-server .
docker run --rm -p 7379:7379 -v lsmkv-data:/data lsmkv-server
```

## Project layout

```
include/lsmkv/   Public headers (KV engine)
include/reldb/   Public headers (relational layer, MVCC + SI)
src/             KV engine implementation
src/reldb/       Relational layer implementation
server/          TCP server (Server class + lsmkv_server main)
tests/           Exhaustive unit + integration tests (incl. server)
examples/        Minimal CLI example
docs/DESIGN.md   KV design notes and PR roadmap
docs/RELATIONAL.md  Relational / MVCC / snapshot-isolation design
Dockerfile       Multi-stage image for the server
docker-compose.yml  Two sample server instances
```

## Relational layer (MVCC + snapshot isolation)

An educational **relational database** with **MVCC** storage and **snapshot isolation**
sits on top of `lsmkv::DB` (see [docs/RELATIONAL.md](docs/RELATIONAL.md)). It does not
change LSM internals.

```cpp
#include "reldb/database.h"
#include "reldb/txn.h"

reldb::Database* db = nullptr;
reldb::Database::Open(options, "/tmp/reldb", &db);
db->CreateTable(schema);

reldb::Transaction* txn = nullptr;
db->Begin(&txn);
txn->Insert("users", row);
txn->Get("users", pk, &row);
txn->Commit();  // or Abort(); Status::Conflict on write-write conflicts
delete txn;
delete db;
```

Guarantees: **snapshot isolation** with first-committer-wins. Tests document that SI
still allows **write skew** (not full serializability). Point lookups by primary key
only in v1 (no SQL parser, no secondary indexes).

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
10. TCP server front-end
11. Docker packaging for the server

## License

MIT
