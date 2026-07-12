#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "lsmkv/common.h"
#include "lsmkv/db.h"
#include "lsmkv/options.h"
#include "lsmkv/status.h"
#include "reldb/catalog.h"
#include "reldb/mvcc.h"
#include "reldb/schema.h"

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
    // Takes mu_ for the Catalog call (Catalog never locks itself).
    lsmkv::Status CreateTable(const TableSchema& schema);

    // Catalog lookup under mu_. Prefer these over catalog()->GetTable when
    // multiple threads share a Database (SQL server, concurrent tests).
    // const: only mu_ (mutable) and the catalog cache are touched.
    lsmkv::Status GetTable(const std::string& name, TableSchema* out) const;
    lsmkv::Status HasTable(const std::string& name, bool* exists) const;

    // Allocates a Transaction with a fresh txn_id and start_ts = last commit.
    // *txn must be empty (get() == nullptr); otherwise InvalidArgument.
    // Caller owns the unique_ptr; Commit() or Abort() before destroy (dtor aborts).
    // Transaction holds a shared_ptr to this Database.
    lsmkv::Status Begin(std::unique_ptr<Transaction>* txn);

    // Raw Catalog pointer. Not thread-safe unless the caller holds mu_ (private)
    // or fully serializes access. Prefer CreateTable / GetTable / HasTable.
    std::shared_ptr<Catalog> catalog() const { return catalog_; }
    std::shared_ptr<MvccStore> store() const { return store_; }
    std::shared_ptr<lsmkv::DB> kv() const { return kv_; }

    lsmkv::Status GetTxnMeta(TxnId id, TxnMeta* out) const;
    lsmkv::Status PutTxnMeta(TxnId id, const TxnMeta& meta);

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

    // Mutable so const GetTable/HasTable can lock (catalog cache updates on miss).
    mutable std::mutex mu_;
    std::shared_ptr<lsmkv::DB> kv_;
    std::shared_ptr<Catalog> catalog_;
    std::shared_ptr<MvccStore> store_;

    Timestamp next_ts_ = 1;
    TxnId next_txn_id_ = 1;
    Timestamp next_version_id_ = 1;
};

}  // namespace reldb
