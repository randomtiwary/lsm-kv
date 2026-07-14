#pragma once

#include <cstddef>
#include <memory>
#include <shared_mutex>
#include <string>

#include "lsmkv/common.h"
#include "lsmkv/db.h"
#include "lsmkv/options.h"
#include "lsmkv/status.h"
#include "reldb/catalog.h"
#include "reldb/mvcc.h"
#include "reldb/schema.h"
#include "reldb/types.h"

namespace reldb {

using Timestamp = lsmkv::Timestamp;

class Transaction;

// Relational database with MVCC + snapshot isolation.
// Owns an underlying lsmkv::DB via shared_ptr (shared with Catalog and MvccStore).
// Database is held as shared_ptr so Transaction / TableRowScan can share it safely.
// Transactions are still unique_ptr (single owner).
class Database : public std::enable_shared_from_this<Database> {
public:
    static lsmkv::Status Open(const lsmkv::Options& options, const std::string& path,
                              std::shared_ptr<Database>* dbptr);

    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // DDL is NOT transactional in v1: applies immediately outside any user
    // Transaction (no snapshot, no Abort rollback). See docs/RELATIONAL.md.
    // Global DDL gate: requires no open user transactions (open_txn_count_ == 0);
    // runs under exclusive mu_ with ddl_in_progress_ set for the duration.
    lsmkv::Status CreateTable(const TableSchema& schema);

    // DROP TABLE: remove catalog entry, then eager-delete d/<name>/ and
    // v/<name>/ prefixes (collect keys, then Delete — no live iterator across
    // Delete). NotFound if the table is missing. Same DDL gate as CreateTable.
    lsmkv::Status DropTable(const std::string& name);

    // ALTER TABLE ADD COLUMN: append col (not PK) with a required non-Null
    // default matching col.type. Collects head PKs, then installs committed
    // rewritten heads (collect-then-rewrite; no txn Begin/Commit under mu_).
    // Same DDL gate as CreateTable. NotFound if the table is missing.
    lsmkv::Status AlterTableAddColumn(const std::string& table, const ColumnDef& col,
                                      const Value& default_value);

    // Catalog lookup. Uses a shared lock on cache hits; upgrades to exclusive
    // only when loading from KV / filling the cache (double-checked).
    lsmkv::Status GetTable(const std::string& name, TableSchema* out) const;
    lsmkv::Status HasTable(const std::string& name, bool* exists) const;

    // Allocates a Transaction with a fresh txn_id and start_ts = last commit.
    // *txn must be empty (get() == nullptr); otherwise InvalidArgument.
    // Caller owns the unique_ptr; Commit() or Abort() before destroy (dtor aborts).
    // Transaction holds a shared_ptr to this Database.
    // Increments open_txn_count_; fails if ddl_in_progress_.
    lsmkv::Status Begin(std::unique_ptr<Transaction>* txn);

    // Live user transactions (Begin without Commit/Abort/dtor finish). In-memory only.
    std::size_t open_txn_count() const;

    std::shared_ptr<MvccStore> store() const { return store_; }
    std::shared_ptr<lsmkv::DB> kv() const { return kv_; }

    lsmkv::Status GetTxnMeta(TxnId id, TxnMeta* out) const;
    lsmkv::Status PutTxnMeta(TxnId id, const TxnMeta& meta);

    // Test-only: set ddl_in_progress_ without running DDL (for Begin gate tests).
    void TEST_SetDdlInProgress(bool v);

private:
    friend class Transaction;
    friend class TableRowScan;

    explicit Database(std::shared_ptr<lsmkv::DB> kv);

    lsmkv::Status InitOracles();
    lsmkv::Status PersistOracles();
    // Crash recovery: finish Committing txns; abort Open txns.
    lsmkv::Status RecoverTxns();
    lsmkv::Status CommitTransaction(Transaction* txn);
    lsmkv::Status AbortTransaction(Transaction* txn);
    lsmkv::Status ApplyCommitWrites(TxnId txn_id, Timestamp commit_ts,
                                    const std::vector<TxnWrite>& writes);
    lsmkv::Status RestoreHeads(const std::vector<TxnWrite>& writes);
    lsmkv::Status RestoreWrittenHeads(Transaction* txn);

    // Under mu_: mark txn finished and decrement open_txn_count_ once.
    void MarkTxnFinishedLocked(Transaction* txn);

    // Shared for concurrent reads (GetTable cache hit, Transaction::Get, scan
    // visibility); exclusive for DDL, Begin/Commit/Abort, and writes.
    // Mutable so const GetTable/HasTable can lock.
    mutable std::shared_mutex mu_;
    std::shared_ptr<lsmkv::DB> kv_;
    std::shared_ptr<Catalog> catalog_;
    std::shared_ptr<MvccStore> store_;

    Timestamp next_ts_ = 1;
    TxnId next_txn_id_ = 1;
    Timestamp next_version_id_ = 1;

    // DDL / txn gate (in-memory; always 0 after Open + RecoverTxns).
    std::size_t open_txn_count_ = 0;
    bool ddl_in_progress_ = false;
};

}  // namespace reldb
