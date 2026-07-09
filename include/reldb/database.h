#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "lsmkv/db.h"
#include "lsmkv/options.h"
#include "lsmkv/status.h"
#include "reldb/catalog.h"
#include "reldb/mvcc.h"
#include "reldb/schema.h"

namespace reldb {

class Transaction;

// Educational relational database with MVCC + snapshot isolation.
// Owns an underlying lsmkv::DB.
class Database {
public:
    static lsmkv::Status Open(const lsmkv::Options& options, const std::string& path,
                              Database** dbptr);

    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    lsmkv::Status CreateTable(const TableSchema& schema);

    // Allocates a Transaction with start_ts = last committed timestamp.
    // Caller owns the pointer and must Commit() or Abort(), then delete.
    lsmkv::Status Begin(Transaction** txn);

    // Exposed for tests / advanced use.
    Catalog* catalog() { return catalog_.get(); }
    MvccStore* store() { return store_.get(); }
    lsmkv::DB* kv() { return kv_; }

private:
    friend class Transaction;

    explicit Database(lsmkv::DB* kv);

    lsmkv::Status InitOracle();
    lsmkv::Status PersistOracle();

    // Called by Transaction::Commit / Abort.
    lsmkv::Status CommitTransaction(Transaction* txn);
    void FinishTransaction(Transaction* txn);

    lsmkv::DB* kv_;  // owned
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<MvccStore> store_;

    // Serializes timestamp allocation and version apply (educational SI).
    std::mutex commit_mu_;
    // Next commit timestamp to assign. Begins at 1. Snapshot for new txns is
    // next_ts_ - 1 (everything committed so far).
    std::uint64_t next_ts_ = 1;
};

}  // namespace reldb
