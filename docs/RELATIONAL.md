# Relational Layer on lsm-kv

**Status:** Active  
**Date:** 2026-07-09  
**Language:** C++17  
**Branch:** `feature/relational-db`

## Overview

This document describes a **simple educational relational database** built on top of
the existing `lsmkv::DB` key-value engine. The layer provides:

- Tables with named columns and a single primary key
- Multi-version concurrency control (**MVCC**) for row storage
- **Snapshot isolation (SI)** for transactions
- A programmatic C++ API (no SQL parser in v1)

Clarity beats performance. The design prefers explicit data structures and tests that
teach *why* SI works the way it does (including what it does **not** prevent).

## Goals

- Correct `CreateTable` / `Insert` / `Get` / `Update` / `Delete` under transactions
- MVCC: readers never block writers; each transaction sees a consistent snapshot
- Snapshot isolation with first-committer-wins write–write conflict detection
- Durable state via the underlying LSM (WAL + SSTables)
- Exhaustive unit tests per component and multi-threaded SI tests

## Non-Goals (v1)

- Full SQL parser / planner / optimizer
- Secondary indexes, foreign keys, joins
- Serializable isolation (SSI / true serializability)
- Range scans / table iterators (point lookups by PK only; needs KV iterators later)
- Distributed transactions, 2PC, replication

## Layering

```
┌─────────────────────────────────────────────┐
│  reldb::Database / reldb::Transaction       │  public API
├─────────────────────────────────────────────┤
│  Catalog  │  TxnManager  │  Table ops       │
├─────────────────────────────────────────────┤
│  Schema / Row codec  │  MVCC version store  │
├─────────────────────────────────────────────┤
│  lsmkv::DB  (Put / Get / Delete)            │  existing engine
└─────────────────────────────────────────────┘
```

The relational layer is a **client** of `lsmkv::DB`. It does not modify LSM internals.
All relational state (catalog, row versions, timestamp oracle) is stored as ordinary
user keys/values.

## Why MVCC on a single-version KV?

`lsmkv::DB` stores one value per user key (plus internal sequence numbers used only
for crash recovery / compaction). Higher-level MVCC is implemented by **encoding
versions as separate keys** and applying a **visibility function** at read time.

```
Physical keys (examples):

  c/t/<table_name>              → TableSchema bytes     (catalog)
  m/next_ts                     → u64 timestamp oracle
  d/<table>/<pk>                → u64 latest_version_ts (row head pointer)
  v/<table>/<pk>/<start_ts>     → VersionRecord bytes   (one version)
```

Point reads walk the version chain via `prev_ts` links (no KV iterator required).

## MVCC version record

```
VersionRecord {
  start_ts   : u64   // commit timestamp that created this version
  end_ts     : u64   // commit timestamp that superseded it; 0 = still live
  prev_ts    : u64   // previous version's start_ts; 0 = none
  flags      : u8    // bit0 = is_tombstone (deleted)
  payload    : bytes // encoded row (empty if tombstone)
}
```

### Visibility (snapshot isolation)

A transaction that began with snapshot timestamp `S` sees version `V` iff:

```
V.start_ts <= S  &&  (V.end_ts == 0 || V.end_ts > S)
```

Among versions of a row, at most one satisfies this for a given `S` (versions form a
non-overlapping lifetime chain). If that version is a tombstone, the row is absent.

### Write path (at commit)

For each primary key in the transaction write set:

1. **Insert:** require no live version at commit time; write new version with
   `start_ts = commit_ts`, `end_ts = 0`.
2. **Update / Delete:** require a live version; set its `end_ts = commit_ts`;
   write a new version (payload or tombstone) with `start_ts = commit_ts`.

The row head pointer `d/<table>/<pk>` always stores the latest `start_ts`.

## Snapshot isolation protocol

Optimistic, first-committer-wins (classic SI):

| Step | Action |
|------|--------|
| **Begin** | `start_ts = read oracle` (does not advance). Snapshot is `start_ts`. |
| **Read** | Apply write-set first (read-your-writes); else MVCC read at `start_ts`. |
| **Write** | Buffer in private write-set (not visible to other txns). |
| **Commit** | Under a commit mutex: allocate `commit_ts = next_ts++`; for each written PK, if another txn committed a newer version with `start_ts > our start_ts`, **abort (conflict)**; else apply versions at `commit_ts`. |
| **Abort** | Discard write-set. |

### What SI guarantees

- Each transaction reads a **consistent snapshot** (no dirty reads, no non-repeatable
  reads of committed data from after `start_ts`).
- Concurrent writers to the **same primary key**: one commits, the other aborts.

### What SI does **not** guarantee

- **Write skew** / full serializability. Classic example: constraint “at least one
  doctor on call” can be broken by two concurrent SI transactions each taking one
  doctor off-call. Teaching tests will document this deliberately.

## Schema model

```
ColumnType = Int64 | String | Bool

ColumnDef  { name, type, primary_key? }
TableSchema { name, columns[] }   // exactly one PK column in v1
Row        { values by column order or name }
```

Rows are length-prefixed field encodings (see implementation). Schema changes after
`CreateTable` are out of scope for v1.

## Public API (sketch)

```cpp
#include "reldb/database.h"

reldb::Database* db = nullptr;
reldb::Database::Open(options, path, &db);

db->CreateTable(schema);

reldb::Transaction* txn = nullptr;
db->Begin(&txn);

txn->Insert("users", row);
txn->Get("users", pk_value, &row);
txn->Update("users", pk_value, row);
txn->Delete("users", pk_value);

txn->Commit();   // or txn->Abort();
delete txn;
delete db;
```

## Concurrency model

| Resource | Sync |
|----------|------|
| Timestamp oracle + commit apply | Single `std::mutex` in `Database` |
| Active transaction objects | Per-txn state only; no shared mutable row cache |
| Underlying `lsmkv::DB` | Existing write mutex + concurrent Get |

Readers of committed data only call `DB::Get` and need no relational lock.
Commits are serialized for simplicity (educational correctness over throughput).

## Testing strategy (high priority)

| Area | Tests |
|------|-------|
| Types / row codec | Round-trip encode/decode, invalid encodings |
| Schema / catalog | Create, persist across reopen, duplicate table errors |
| MVCC visibility | Pure unit tests: chains, tombstones, snapshot boundaries |
| Single-thread txn | Insert/get/update/delete, abort discards writes, read-your-writes |
| SI conflicts | Two txns update same PK → one Conflict |
| SI snapshots | T1 reads x; T2 commits change to x; T1 still sees old value |
| Concurrency stress | Many threads insert/update distinct and overlapping PKs |
| Write skew doc | Explicit test showing SI allows write skew (educational) |

## PR Plan

Work lands on `feature/relational-db` via small PRs, then the feature branch merges to
`main`.

### PR 12: Design + library scaffold
- **Files:** `docs/RELATIONAL.md`, `docs/DESIGN.md` (pointer), CMake `src/reldb/`,
  `include/reldb/`, smoke test
- **Description:** Document the architecture; wire an empty `reldb` source set so later
  PRs only add code.

### PR 13: Types, row codec, table schema
- **Files:** `types`, `row`, `schema` headers/sources + unit tests
- **Description:** Column types, `Value`, `Row` encode/decode, `TableSchema` validation.

### PR 14: Catalog persistence
- **Files:** `catalog` + tests with real `lsmkv::DB`
- **Description:** Store/load table schemas under `c/t/<name>` keys.

### PR 15: MVCC version store + visibility
- **Files:** `mvcc` key layout, `VersionRecord`, visibility helpers + pure tests
- **Description:** Version chain read/write helpers (no transactions yet).

### PR 16: Transactions + Database API
- **Files:** `txn`, `database`, conflict detection, Status conflict code + tests
- **Description:** Begin/Commit/Abort, Insert/Get/Update/Delete, SI single-thread cases.

### PR 17: Snapshot isolation concurrency tests
- **Files:** multi-threaded SI tests, write-skew educational test, README blurb
- **Description:** Stress overlapping commits; document isolation boundaries.

## Open questions

None for v1 — defaults favor the simplest SI implementation that is still correct and
easy to review.
