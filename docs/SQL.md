# SQL Layer on reldb

**Status:** Implemented on branch `feature/sql-layer` (see `reldb::SqlSession`, `reldb_sql_example`)  
**Date:** 2026-07-11  
**Language:** C++17  

## Overview

Add an educational **SQL frontend** on top of the existing reldb MVCC + snapshot-isolation
stack. Multi-row queries need **scans**, which need a public **KV iterator**. A **basic
optimizer** (rule-based access-path selection) sits between SQL binding and physical
execution.

Clarity over completeness: a small dialect, explicit ASTs, volcano-style operators, and
tests that show *why* a plan chose a point lookup vs a table scan.

### Branching

| Branch | Role |
|--------|------|
| `feature/sql-layer` | Integration branch for this work (forked from `main` post-#43) |
| `review/sql-*` | Small PRs **targeting `feature/sql-layer` only** (not `main`) |

`feature/sql-layer` merges to `main` once the stack is reviewable end-to-end.

## Goals (v1)

- SQL **DDL**: `CREATE TABLE`, `DROP TABLE` (still **non-transactional**, same as
  `Database::CreateTable` / `Database::DropTable`)
- SQL **txn control**: `BEGIN`, `COMMIT`, `ABORT` / `ROLLBACK` (thin wrappers over reldb txn API)
- SQL **DML**: `INSERT`, `SELECT`, `UPDATE`, `DELETE` with simple `WHERE` / `ORDER BY` / `LIMIT`
- **Scan** support under SI (same visibility as `Transaction::Get`)
- **Basic optimizer**: PK equality → point get; PK range → range scan; else seq scan + filter
- Programmatic entry point: execute one or more statements; session holds optional open txn

## Non-goals (v1)

- Joins, subqueries, CTEs, views, window functions
- Aggregates (`GROUP BY`, `COUNT`, …)
- Secondary indexes, foreign keys
- Cost-based optimization, statistics collection
- Prepared statements / bind parameters (stretch; not required)
- Full SQL standard / Postgres compatibility
- Changing LSM internals beyond a public `DB` iterator
- Serializable isolation

**Roadmap (post-v1):** SQL network server, `DROP` / `ALTER TABLE`, aggregates, and
joins are planned in [ROADMAP_SERVER_DDL_AGG_JOIN.md](ROADMAP_SERVER_DDL_AGG_JOIN.md)
(build order: server → DDL → aggregates → joins).

## Layering

```
┌──────────────────────────────────────────────────────────┐
│  SQL session / Execute(sql) → Result                     │  new
├──────────────────────────────────────────────────────────┤
│  Parser → AST → Binder → Logical plan → (rules) → Phys   │  new
├──────────────────────────────────────────────────────────┤
│  Executors: Scan / Filter / Project / Sort / Limit / DML │  new
├──────────────────────────────────────────────────────────┤
│  reldb::Transaction  (Get, Scan, Insert, Update, Delete) │  extend
├──────────────────────────────────────────────────────────┤
│  MvccStore + Catalog                                     │  extend scan
├──────────────────────────────────────────────────────────┤
│  lsmkv::DB + Iterator  (user-key ordered scan)           │  new public API
└──────────────────────────────────────────────────────────┘
```

The SQL layer is a **client** of reldb. reldb remains usable without SQL.

## Prerequisite: KV iterator

`lsmkv::DB` today exposes only `Put` / `Get` / `Delete`. SSTable and SkipList iterators
are internal. Table scan needs ordered iteration over user keys.

### Proposed API (sketch)

```cpp
// Owned by caller (unique_ptr). Snapshot consistent with Get at iterator creation.
std::unique_ptr<Iterator> DB::NewIterator(const ReadOptions& options);

class Iterator {
 public:
  bool Valid() const;
  void SeekToFirst();
  void Seek(const Slice& target);  // first key >= target
  void Next();
  Slice key() const;    // user key
  Slice value() const;  // latest live value for that user key
};
```

**Semantics (v1):** same visibility as `Get` for the sequence number / snapshot captured
when the iterator is created (document exact ReadOptions fields in the iterator PR).
No concurrent mutation guarantees beyond existing DB rules.

**Prefix scans for reldb:** seek to `d/<table>/` and stop when the key leaves that prefix.

## reldb scan API

Row heads are stored at `d/<table>/<pk_hex>`. Scan:

1. Iterate heads with prefix `d/<table>/` (optional start/end PK bounds).
2. Decode PK from key suffix.
3. Resolve visible row via existing MVCC `GetRow` (own provisionals + snapshot).
4. Skip not-found / deleted.

### Proposed API (sketch)

```cpp
// Cursor over rows visible to this transaction. unique_ptr ownership.
class TableRowScan {
 public:
  bool Valid() const;
  void Next();
  const Value& pk() const;
  const Row& row() const;
};

// start_pk / end_pk optional; empty = unbounded. Range is half-open or closed —
// pick one in the scan PR and test it (recommend: start inclusive, end exclusive
// when end is set; both inclusive for equality-only range).
Status Transaction::Scan(const std::string& table,
                         const Value* start_pk,  // nullable
                         const Value* end_pk,    // nullable
                         std::unique_ptr<TableRowScan>* out);
```

SI: all rows seen are consistent with `start_ts_` and `txn_id_` (same as `Get`).

## SQL session and BEGIN / COMMIT

v1 includes SQL transaction statements. Keep the model simple:

### Session state

```cpp
class SqlSession {
 public:
  explicit SqlSession(Database* db);  // non-owning

  // Run one statement or a script separated by ';'.
  Status Execute(std::string_view sql, QueryResult* result);

  bool InTransaction() const;
  // Optional: expose current txn for mixed C++/SQL use
  Transaction* current_txn();  // non-owning; nullptr if none
};
```

| Statement | Effect |
|-----------|--------|
| `BEGIN` | If already in a txn → error. Else `Database::Begin` and store `unique_ptr<Transaction>`. |
| `COMMIT` | If no txn → error. Else `Commit()`, release txn. |
| `ABORT` / `ROLLBACK` | If no txn → error. Else `Abort()`, release txn. |
| DML / `SELECT` | If no open txn → **auto-txn**: begin, run, commit (or abort on error). If open txn → run inside it. |
| `CREATE TABLE` / `DROP TABLE` | See **DDL is forbidden inside a transaction** below. |

**Autocommit for single statements** avoids forcing `BEGIN` for every demo, while
`BEGIN`…`COMMIT` enables multi-statement SI snapshots.

```text
BEGIN;
INSERT INTO t VALUES (1, 'a');
SELECT * FROM t;          -- sees insert (same txn)
COMMIT;
```

Conflict on `COMMIT` → surface `Status::Conflict`; session txn is finished (same as
C++ API after failed commit).

### DDL is forbidden inside a transaction

DDL (`CREATE TABLE`, `DROP TABLE`; later `ALTER`) is **non-transactional** in reldb: it
applies immediately and is **not** rolled back by `ABORT`. Allowing it inside
`BEGIN`…`COMMIT` would be misleading (looks transactional, is not).

**Rule (v1, locked):**

- If `SqlSession` currently has an open transaction (`InTransaction() == true`), any
  DDL statement must fail with a clear `InvalidArgument` (or similar) **before**
  mutating the catalog.
- DDL is only allowed when the session is **not** in a transaction.
- Outside a txn, `CREATE TABLE` / `DROP TABLE` go through `Database::CreateTable` /
  `Database::DropTable` (immediate; Database also requires no open user txns).

```text
BEGIN;
CREATE TABLE t(...);   -- ERROR: DDL not allowed inside a transaction
DROP TABLE t;          -- ERROR: same session gate
ABORT;

CREATE TABLE t(...);   -- OK (no open txn)
DROP TABLE t;          -- OK (no open txn)
```

**Session gate** is the clear error for the common `BEGIN; CREATE/DROP` mistake; the
Database global DDL gate additionally blocks multi-client races.

## SQL dialect (v1)

### Types

Map to existing `ColumnType`: `INT` / `INTEGER` → Int64, `TEXT` / `STRING` → String,
`BOOL` / `BOOLEAN` → Bool. One primary key column per table (match catalog rules).

### Grammar (informal)

```
script        := statement ( ';' statement )* ';'?
statement     := begin_stmt | commit_stmt | abort_stmt
               | create_table | drop_table | insert_stmt | select_stmt
               | update_stmt | delete_stmt

begin_stmt    := BEGIN [ TRANSACTION ]
commit_stmt   := COMMIT [ TRANSACTION ]
abort_stmt    := ABORT | ROLLBACK [ TRANSACTION ]

create_table  := CREATE TABLE name '(' col_def (',' col_def)* ')'
drop_table    := DROP TABLE name
col_def       := name type [ PRIMARY KEY ]

insert_stmt   := INSERT INTO name [ '(' names ')' ] VALUES '(' literals ')'
select_stmt   := SELECT select_list FROM name
                 [ WHERE expr ]
                 [ ORDER BY order_item (',' order_item)* ]
                 [ LIMIT integer ]
update_stmt   := UPDATE name SET assign (',' assign)* [ WHERE expr ]
delete_stmt   := DELETE FROM name [ WHERE expr ]

select_list   := '*' | expr (',' expr)*   -- v1: column names or *
assign        := name '=' expr
order_item    := name [ ASC | DESC ]

expr          := or_expr
or_expr       := and_expr ( OR and_expr )*
and_expr      := not_expr ( AND not_expr )*
not_expr      := NOT not_expr | cmp_expr
cmp_expr      := add_expr ( cmp_op add_expr )?
cmp_op        := '=' | '!=' | '<>' | '<' | '<=' | '>' | '>='
add_expr      := primary   -- no arithmetic in v1 beyond literals
primary       := literal | name | '(' expr ')'
literal       := INTEGER | STRING | TRUE | FALSE | NULL
```

Unsupported constructs must fail at parse or bind with a clear error (not silently ignore).

### WHERE without predicate

- `UPDATE` / `DELETE` **without** `WHERE`: allowed (full table); document danger in docs/example.
- Prefer tests cover both filtered and unfiltered forms.

## Execution model

### Logical operators (post-bind)

- `LogicalCreateTable`, `LogicalBegin`, `LogicalCommit`, `LogicalAbort`
- `LogicalInsert`, `LogicalUpdate`, `LogicalDelete`
- `LogicalScan` (table + optional PK bounds)
- `LogicalFilter`, `LogicalProject`, `LogicalSort`, `LogicalLimit`

### Physical operators (volcano / pull)

- `PkPointGet`, `PkRangeScan`, `SeqScan`
- `Filter`, `Project`, `Sort`, `Limit`
- `InsertOp`, `UpdateOp`, `DeleteOp`
- Session ops: begin / commit / abort

Each operator is a small class with `Next()` or one-shot `Execute()` for DDL/DML/txn.

### Result

```cpp
struct QueryResult {
  std::vector<std::string> column_names;
  std::vector<Row> rows;           // SELECT
  uint64_t rows_affected = 0;      // DML
  // Optional: plan_tag / explain string for tests
};
```

`BEGIN`/`COMMIT`/`ABORT` return empty result on success.

## Basic optimizer (v1)

No statistics. After bind:

1. **Normalize** boolean expressions lightly (optional).
2. **Access path** for a single-table `FROM t` + `WHERE e`:
   - If `e` implies `pk = const` → `PkPointGet`.
   - Else if `e` is only comparisons on `pk` forming a range → `PkRangeScan`.
   - Else → `SeqScan` + `Filter(e)` (push residual predicates into filter).
3. **Project** after filter (or at scan if only needed columns — optional).
4. **Sort** then **Limit** if present.
5. **UPDATE/DELETE**: same access-path rules to find target PKs, then apply writes
   through the open transaction.

Expose a test-only or `EXPLAIN` string (e.g. `PLAN: PkPointGet`) so tests assert the
chosen path without hard-coding operator pointers.

## Public C++ API (sketch)

```cpp
#include "reldb/sql.h"

std::shared_ptr<reldb::Database> db;
reldb::Database::Open(opt, path, &db);

reldb::SqlSession session(db.get());
reldb::QueryResult result;
EXPECT_OK(session.Execute(
    "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);", &result));
EXPECT_OK(session.Execute("BEGIN;", &result));
EXPECT_OK(session.Execute(
    "INSERT INTO users(id, name) VALUES (1, 'ada');", &result));
EXPECT_OK(session.Execute("SELECT * FROM users WHERE id = 1;", &result));
EXPECT_OK(session.Execute("COMMIT;", &result));
```

Mixed use remains valid: C++ `Begin`/`Commit` for some apps; SQL session for others.
Do not require both to interleave on the same session object in v1 (document if
`current_txn()` is exposed).

## Testing strategy

| Area | Tests |
|------|-------|
| DB iterator | Empty, multi-key order, seek, matches Get for live keys |
| reldb Scan | Empty table, N rows, PK order, SI snapshot during concurrent commit |
| Expressions | Eval true/false/null edge cases for compare/logic |
| Executors | Hand-built plans without parser |
| Parser | Valid subset ASTs; reject joins/aggregates |
| Bind + optimizer | `WHERE id = 1` → point; residual → scan+filter (`EXPLAIN`) |
| SQL e2e | BEGIN/INSERT/SELECT/COMMIT; autocommit SELECT; conflict on commit |
| Session | Double BEGIN errors; COMMIT without BEGIN errors |
| DDL vs txn | `BEGIN` + `CREATE`/`DROP TABLE` → error, catalog unchanged; DDL OK with no open txn |

## PR plan (all target `feature/sql-layer`)

| PR | Title | Deliverable |
|----|--------|-------------|
| **00** | Design + pointers | This doc; links from `RELATIONAL.md` / `DESIGN.md` |
| **01** | KV `DB::Iterator` | Public iterator + tests |
| **02** | reldb `Scan` | `Transaction::Scan` / `TableRowScan` + SI tests |
| **03** | Expr + `QueryResult` | AST eval, result types + unit tests |
| **04** | Physical executors | Operators + hand-built plan tests |
| **05** | Lexer + parser | SQL text → AST + tests |
| **06** | Binder + optimizer + `SqlSession` | Bind, rules, `Execute`, BEGIN/COMMIT/ABORT, e2e SQL |
| **06b** (optional split) | Block DDL in open txn | Session rejects `CREATE TABLE` (etc.) when `InTransaction()`; tests |
| **07** | Example + README | `reldb_sql_example`, user-facing docs |

PR **06b** may be folded into **06** if small; either way the gate is required before
calling the feature branch “SQL-complete.”

Dependency chain:

```text
00 ──► 01 ──► 02 ──► 03 ──► 04 ──► 05 ──► 06 ──► 07
                      └──────────┘
                 (03 can start once 00 is agreed;
                  05 can parallelize after 00 if staffing allows,
                  but 06 needs 02–05)
```

## Open decisions (defaults locked for v1)

| Topic | Default |
|-------|---------|
| SQL `BEGIN`/`COMMIT`/`ABORT` | **In v1** (this doc) |
| Autocommit when no txn | **Yes** for DML/SELECT |
| `CREATE TABLE` / DDL inside open SQL txn | **Hard error** (DDL non-transactional; see section above) |
| `ORDER BY` | In-memory sort only |
| Parser | Hand-written recursive descent |
| Optimizer | Rule-based only; optional `EXPLAIN` |

## Out of scope follow-ups

See [ROADMAP_SERVER_DDL_AGG_JOIN.md](ROADMAP_SERVER_DDL_AGG_JOIN.md) for the ordered
plan (SQL server → `DROP`/`ALTER` → aggregates → joins). Still out of that roadmap:

- Prepared statements  
- Secondary indexes  
- Cost-based optimizer / join reordering  
- Auth/TLS, Postgres wire protocol

