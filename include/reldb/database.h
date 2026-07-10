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
// This PR: open DB, catalog, txn registry, Begin / read-only Get / empty Commit / Abort.
// Eager writes land in the follow-up PR.
class Database {
public:
    static lsmkv::Status Open(const lsmkv::Options& options, const std::string& path,
                              Database** dbptr);

    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // DDL is NOT transactional in v1: it commits immediately outside any
    // user Transaction (no snapshot, no rollback with Abort). See docs/RELATIONAL.md.
    lsmkv::Status CreateTable(const TableSchema& schema);

    lsmkv::Status Begin(Transaction** txn);

    std::shared_ptr<Catalog> catalog() const { return catalog_; }
    std::shared_ptr<MvccStore> store() const { return store_; }
    std::shared_ptr<lsmkv::DB> kv() const { return kv_; }

    lsmkv::Status GetTxnMeta(TxnId id, TxnMeta* out) const;
    lsmkv::Status PutTxnMeta(TxnId id, const TxnMeta& meta);

private:
    friend class Transaction;

    explicit Database(std::shared_ptr<lsmkv::DB> kv);

    lsmkv::Status InitOracles();
    lsmkv::Status PersistOracles();
    lsmkv::Status CommitTransaction(Transaction* txn);
    lsmkv::Status AbortTransaction(Transaction* txn);

    std::mutex mu_;
    std::shared_ptr<lsmkv::DB> kv_;
    std::shared_ptr<Catalog> catalog_;
    std::shared_ptr<MvccStore> store_;

    Timestamp next_ts_ = 1;
    TxnId next_txn_id_ = 1;
    Timestamp next_version_id_ = 1;
};

}  // namespace reldb
