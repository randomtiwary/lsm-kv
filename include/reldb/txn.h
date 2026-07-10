#pragma once

#include <string>

#include "lsmkv/common.h"
#include "lsmkv/status.h"
#include "reldb/mvcc.h"
#include "reldb/row.h"
#include "reldb/types.h"

namespace reldb {

using Timestamp = lsmkv::Timestamp;

class Database;

// Snapshot-isolated transaction (registry + reads in this PR; writes next).
class Transaction {
public:
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Timestamp start_ts() const { return start_ts_; }
    TxnId txn_id() const { return txn_id_; }
    bool finished() const { return finished_; }

    lsmkv::Status Get(const std::string& table, const Value& pk, Row* out);
    lsmkv::Status Commit();
    lsmkv::Status Abort();

private:
    friend class Database;

    Transaction(Database* db, TxnId txn_id, Timestamp start_ts);

    Database* db_;
    TxnId txn_id_;
    Timestamp start_ts_;
    bool finished_ = false;
};

}  // namespace reldb
