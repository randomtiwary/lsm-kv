#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "lsmkv/status.h"
#include "reldb/row.h"
#include "reldb/types.h"

namespace reldb {

class Database;

// Snapshot-isolated transaction. Reads use start_ts; writes buffer until Commit.
class Transaction {
public:
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    std::uint64_t start_ts() const { return start_ts_; }
    bool finished() const { return finished_; }

    lsmkv::Status Insert(const std::string& table, const Row& row);
    lsmkv::Status Update(const std::string& table, const Row& row);
    lsmkv::Status Delete(const std::string& table, const Value& pk);
    lsmkv::Status Get(const std::string& table, const Value& pk, Row* out);

    lsmkv::Status Commit();
    lsmkv::Status Abort();

private:
    friend class Database;

    enum class WriteOp : std::uint8_t { kInsert, kUpdate, kDelete };

    struct WriteEntry {
        WriteOp op;
        std::string table;
        Value pk;
        Row row;  // valid for Insert/Update
    };

    Transaction(Database* db, std::uint64_t start_ts);

    static std::string WriteKey(const std::string& table, const Value& pk);

    Database* db_;
    std::uint64_t start_ts_;
    bool finished_ = false;
    // Ordered map for deterministic commit apply.
    std::map<std::string, WriteEntry> write_set_;
};

}  // namespace reldb
