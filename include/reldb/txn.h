#pragma once

#include <string>
#include <vector>

#include "lsmkv/common.h"
#include "lsmkv/status.h"
#include "reldb/mvcc.h"
#include "reldb/row.h"
#include "reldb/types.h"

namespace reldb {

using Timestamp = lsmkv::Timestamp;

class Database;

// Snapshot-isolated transaction with eager durable writes.
//
// Writes install provisional VersionRecords immediately (cost on the write
// path). Commit stamps commit_ts / closes prior versions and marks the txn
// committed. Abort marks the txn aborted and restores row heads when needed.
//
// Early write-write conflict: if another open txn already has a provisional
// version at the head of a PK chain, this txn gets Status::Conflict on write.
class Transaction {
public:
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Timestamp start_ts() const { return start_ts_; }
    TxnId txn_id() const { return txn_id_; }
    bool finished() const { return finished_; }

    lsmkv::Status Insert(const std::string& table, const Row& row);
    lsmkv::Status Update(const std::string& table, const Row& row);
    lsmkv::Status Delete(const std::string& table, const Value& pk);

    // Point lookup by primary key only (v1).
    // Multi-row query results / table scans / secondary indexes are intentionally
    // out of scope until the KV layer exposes iterators and we add a scan API.
    lsmkv::Status Get(const std::string& table, const Value& pk, Row* out);

    lsmkv::Status Commit();
    lsmkv::Status Abort();

private:
    friend class Database;

    struct WrittenKey {
        std::string table;
        Value pk;
        Timestamp version_id;
    };

    Transaction(Database* db, TxnId txn_id, Timestamp start_ts);

    lsmkv::Status Write(const std::string& table, const Value& pk, bool is_delete,
                        const Row* row, bool is_insert);

    Database* db_;
    TxnId txn_id_;
    Timestamp start_ts_;
    bool finished_ = false;
    std::vector<WrittenKey> written_;
};

}  // namespace reldb
