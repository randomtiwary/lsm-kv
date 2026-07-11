#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lsmkv/common.h"
#include "lsmkv/db.h"
#include "lsmkv/status.h"
#include "reldb/mvcc.h"
#include "reldb/row.h"
#include "reldb/types.h"

namespace reldb {

using Timestamp = lsmkv::Timestamp;

class Database;

// Cursor over rows of one table visible to a transaction (SI snapshot).
// Owned by the caller via unique_ptr. Holds shared_ptr<Database> so the DB
// outlives the scan even if the caller's Database handle is released.
//
// Destroy the scan before Commit/Abort/further writes on the same transaction:
// the underlying DB iterator may hold shared locks that block writers.
// Not valid after the Transaction finishes.
class TableRowScan {
public:
    ~TableRowScan();

    TableRowScan(const TableRowScan&) = delete;
    TableRowScan& operator=(const TableRowScan&) = delete;

    bool Valid() const { return valid_; }
    void Next();
    const Value& pk() const { return pk_; }
    const Row& row() const { return row_; }
    lsmkv::Status status() const { return status_; }

private:
    friend class Transaction;

    TableRowScan(std::shared_ptr<Database> db, TxnId txn_id, Timestamp start_ts,
                 std::string table, std::string prefix, std::string end_key, bool has_end,
                 std::unique_ptr<lsmkv::Iterator> it);

    // Advance KV iterator to the next head key in range with a visible live row.
    lsmkv::Status Advance();

    std::shared_ptr<Database> db_;
    TxnId txn_id_;
    Timestamp start_ts_;
    std::string table_;
    std::string prefix_;   // "d/<table>/"
    std::string end_key_;  // exclusive upper bound when has_end_
    bool has_end_ = false;
    std::unique_ptr<lsmkv::Iterator> it_;
    bool valid_ = false;
    Value pk_;
    Row row_;
    lsmkv::Status status_;
};

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

    // Point lookup by primary key.
    lsmkv::Status Get(const std::string& table, const Value& pk, Row* out);

    // Scan rows visible to this transaction in KV key order (encoded PK order).
    // start_pk inclusive when non-null; end_pk exclusive when non-null.
    // Both null: full table. Caller owns *out.
    lsmkv::Status Scan(const std::string& table, const Value* start_pk, const Value* end_pk,
                       std::unique_ptr<TableRowScan>* out);

    lsmkv::Status Commit();
    lsmkv::Status Abort();

    // Crash-recovery tests only: mark finished and drop the Database reference
    // without Abort/Commit (simulates process death leaving Open provisionals).
    void TEST_AbandonWithoutAbort();

private:
    friend class Database;
    friend class TableRowScan;

    struct WrittenKey {
        std::string table;
        Value pk;
        Timestamp version_id;
    };

    Transaction(std::shared_ptr<Database> db, TxnId txn_id, Timestamp start_ts);

    lsmkv::Status Write(const std::string& table, const Value& pk, bool is_delete,
                        const Row* row, bool is_insert);

    std::shared_ptr<Database> db_;
    TxnId txn_id_;
    Timestamp start_ts_;
    bool finished_ = false;
    std::vector<WrittenKey> written_;
};

}  // namespace reldb
