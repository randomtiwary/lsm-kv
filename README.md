# lsm-kv — Educational Embedded SQL Database (C++)

An educational **embedded SQL database** in modern **C++17**: tables, transactions
with **MVCC** and **snapshot isolation**, a small **SQL dialect**, and an interactive
shell — all built on a from-scratch **LSM-tree key-value** storage engine.

Clarity over completeness. The goal is code you can read, review, and modify while
learning how real systems (LevelDB/RocksDB-style storage + simple relational/SQL
layers) fit together.

## What you get

| Layer | Capabilities |
|-------|----------------|
| **SQL** | `CREATE TABLE`, `INSERT` / `SELECT` / `UPDATE` / `DELETE`, `BEGIN` / `COMMIT` / `ABORT`, simple `WHERE` / `ORDER BY` / `LIMIT` |
| **Relational (C++)** | Schemas, rows, `Transaction` with SI, point get + table scan |
| **Storage (LSM KV)** | WAL, MemTable (SkipList), SSTables, flush/compaction, crash recovery |
| **Tools** | Interactive SQL shell, demos, optional TCP server for raw KV |

**Not** a production Postgres/MySQL clone: no joins, aggregates, secondary indexes,
or cost-based optimizer. See [docs/SQL.md](docs/SQL.md) and
[docs/RELATIONAL.md](docs/RELATIONAL.md). Planned next layers (SQL server, richer DDL,
aggregates, joins): [docs/ROADMAP_SERVER_DDL_AGG_JOIN.md](docs/ROADMAP_SERVER_DDL_AGG_JOIN.md).

## Quick start

```bash
# Prerequisites: CMake ≥ 3.16, C++17 compiler; SQL shell also needs libreadline
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# Interactive SQL (default DB dir: /tmp/reldb_sql_shell)
./scripts/run_sql_shell.sh
# or: ./scripts/run_sql_shell.sh --db /path/to/db
```

```text
sql> CREATE TABLE users(id INT PRIMARY KEY, name TEXT, score INT);
sql> BEGIN;
sql> INSERT INTO users VALUES (1, 'ada', 10);
sql> INSERT INTO users VALUES (2, 'bob', 30);
sql> SELECT * FROM users WHERE id = 1;
plan: PkPointGet
+----+------+-------+
| id | name | score |
+----+------+-------+
| 1  | ada  | 10    |
+----+------+-------+
(1 row)
sql> COMMIT;
```

Other demos:

```bash
./scripts/run_sql_example.sh   # scripted SQL walkthrough
./scripts/run_example.sh       # raw LSM KV Put/Get demo
```

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  SQL shell / SqlSession::Execute(sql)                    │
├──────────────────────────────────────────────────────────┤
│  Parser → bind → access path → volcano executors         │
├──────────────────────────────────────────────────────────┤
│  reldb::Database / Transaction  (MVCC + snapshot SI)     │
├──────────────────────────────────────────────────────────┤
│  lsmkv::DB  (WAL, MemTable, SSTables, compaction)        │
└──────────────────────────────────────────────────────────┘
```

### SQL frontend

`reldb::SqlSession` parses a small dialect, binds against the catalog, picks a simple
access path (`PkPointGet` when `WHERE pk = const`, otherwise scan + residual filter),
and runs volcano-style operators. Design notes: [docs/SQL.md](docs/SQL.md).

- **DDL** (`CREATE TABLE`) is **non-transactional** and is **rejected** while a SQL
  transaction is open.
- **DML / SELECT** without `BEGIN` use **autocommit**.
- `QueryResult.plan_tag` shows the chosen plan (useful for learning and tests).

```cpp
#include "reldb/database.h"
#include "reldb/query_result.h"
#include "reldb/sql_session.h"

std::shared_ptr<reldb::Database> db;
reldb::Database::Open(options, "/tmp/reldb_sql", &db);

reldb::SqlSession session(db);
reldb::QueryResult result;
session.Execute("CREATE TABLE users(id INT PRIMARY KEY, name TEXT);", result);
session.Execute("BEGIN;", result);
session.Execute("INSERT INTO users(id, name) VALUES (1, 'ada');", result);
session.Execute("SELECT * FROM users WHERE id = 1;", result);  // plan_tag: PkPointGet
session.Execute("COMMIT;", result);
```

### Relational API (C++, no SQL)

Same storage and isolation model, without the parser:

```cpp
#include "reldb/database.h"
#include "reldb/txn.h"

std::shared_ptr<reldb::Database> db;
reldb::Database::Open(options, "/tmp/reldb", &db);
db->CreateTable(schema);

std::unique_ptr<reldb::Transaction> txn;
db->Begin(&txn);
txn->Insert("users", row);
txn->Get("users", pk, &row);
txn->Scan("users", nullptr, nullptr, &scan);  // SI-visible row cursor
txn->Commit();  // or Abort(); Status::Conflict on write-write conflicts
```

**Snapshot isolation** with first-committer-wins. SI still allows **write skew** (not
full serializability) — intentional and covered by tests. No secondary indexes.

### Storage engine (LSM key-value)

The relational layer persists through an educational LSM engine (`lsmkv::DB`). Details
in [docs/DESIGN.md](docs/DESIGN.md).

```
Put/Delete ──► WAL ──► MemTable (SkipList)
                           │ flush
                           ▼
                     SSTable L0 (overlapping)
                           │ compact
                           ▼
                     SSTable L1 (non-overlapping ranges)

Get ──► MemTable ──► Immutable MemTable ──► L0 ──► L1
```

| Component | Role |
|-----------|------|
| **Status / Slice** | Error reporting and non-owning byte views |
| **SkipList** | Concurrent in-memory ordered map |
| **MemTable** | Sequence numbers and tombstones |
| **WAL** | Crash recovery |
| **Block / SSTable** | Sorted on-disk tables |
| **Version / Manifest** | Live file set across levels |
| **DB** | `Put` / `Get` / `Delete` / `NewIterator`, flush & compaction |

```cpp
#include "lsmkv/db.h"

lsmkv::Options options;
options.create_if_missing = true;
lsmkv::DB* db = nullptr;
lsmkv::DB::Open(options, "/tmp/mydb", &db);
db->Put(lsmkv::WriteOptions(), "hello", "world");
std::string value;
db->Get(lsmkv::ReadOptions(), "hello", &value);
delete db;
```

**Concurrency (KV layer):** writes are serialized; reads use shared locks / snapshots;
a background thread flushes and compacts.

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

| Target / script | Purpose |
|-----------------|---------|
| `./scripts/run_sql_shell.sh` | Interactive SQL (needs **libreadline**) |
| `./scripts/run_sql_example.sh` | Scripted SQL demo |
| `./scripts/run_example.sh` | LSM KV Put/Get demo |
| `reldb_example` | C++ transactional API demo |

Requirements: CMake ≥ 3.16, C++17 (GCC/Clang). SQL shell: `libreadline-dev` (or equivalent).

## Optional: TCP server (raw KV)

Line-oriented TCP front-end for the **KV** engine only (`GET` / `SET` / `DEL`), not SQL.
Default port **7379**.

```bash
./scripts/run_server.sh --db /tmp/lsmkv_data --port 7379 --max-clients 128
printf 'SET hello world\nGET hello\nQUIT\n' | nc -q 1 127.0.0.1 7379
```

| Request | Response |
|---------|----------|
| `PING` | `+PONG` |
| `SET <key> <value>` | `+OK` |
| `GET <key>` | `$N` + value line, or `NOT_FOUND` |
| `DEL <key>` | `+OK` |
| `QUIT` | `+OK` (closes connection) |

Docker:

```bash
docker compose up --build
docker build -t lsmkv-server .
docker run --rm -p 7379:7379 -v lsmkv-data:/data lsmkv-server
```

## Project layout

```
include/reldb/     Relational + SQL public headers (Database, SqlSession, …)
include/lsmkv/     LSM KV engine public headers
src/reldb/         MVCC, transactions, parser, executors, session
src/               LSM engine implementation
examples/          reldb_sql_shell, reldb_sql_example, reldb_example, lsmkv_example
scripts/           run_sql_shell.sh, run_sql_example.sh, …
tests/             Unit and integration tests
docs/SQL.md        SQL dialect, plans, session behavior
docs/RELATIONAL.md MVCC + snapshot isolation design
docs/DESIGN.md     LSM storage design
server/            Optional TCP KV server
```

## License

MIT
