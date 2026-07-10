#pragma once

#include <cstdint>
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
class Database {
public:
    static lsmkv::Status Open(const lsmkv::Options& options, const std::string& path,
                              Database** dbptr);

    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    lsmkv::Status CreateTable(const TableSchema& schema);

    // Allocates a Transaction with a fresh txn_id and start_ts = last commit.
    // Caller owns the pointer and must Commit() or Abort(), then delete.
    lsmkv::Status Begin(Transaction** txn);

    Catalog* catalog() { return catalog_.get(); }
    MvccStore* store() { return store_.get(); }
    lsmkv::DB* kv() { return kv_.get(); }

    // Txn registry (also used by MvccStore visibility).
    lsmkv::Status GetTxnMeta(TxnId id, TxnMeta* out) const;
    lsmkv::Status PutTxnMeta(TxnId id, const TxnMeta& meta);

private:
    friend class Transaction;

    explicit Database(std::shared_ptr<lsmkv::DB> kv);

    lsmkv::Status InitOracles();
    lsmkv::Status PersistOracles();

    lsmkv::Status CommitTransaction(Transaction* txn);
    lsmkv::Status AbortTransaction(Transaction* txn);
    lsmkv::Status RestoreWrittenHeads(Transaction* txn);

    // Serialize begin/write/commit/abort so head + provisional WW checks are safe.
    std::mutex mu_;

    std::shared_ptr<lsmkv::DB> kv_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<MvccStore> store_;

    // next_ts_: next commit timestamp (snapshots are next_ts_ - 1).
    // next_txn_id_ / next_version_id_: allocation counters.
    Timestamp next_ts_ = 1;
    TxnId next_txn_id_ = 1;
    Timestamp next_version_id_ = 1;
};

}  // namespace reldb
