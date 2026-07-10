#include "reldb/txn.h"

#include "reldb/database.h"
#include "reldb/mvcc.h"
#include "reldb/schema.h"

namespace reldb {

Transaction::Transaction(Database* db, Timestamp start_ts)
    : db_(db), start_ts_(start_ts) {}

Transaction::~Transaction() {
    if (!finished_) {
        // Best-effort abort on destroy so write-set is dropped.
        Abort();
    }
}

std::string Transaction::WriteKey(const std::string& table, const Value& pk) {
    return table + "/" + EncodePkForKey(pk);
}

lsmkv::Status Transaction::Insert(const std::string& table, const Row& row) {
    if (finished_) {
        return lsmkv::Status::InvalidArgument("transaction finished");
    }
    TableSchema schema;
    auto st = db_->catalog()->GetTable(table, &schema);
    if (!st.ok()) return st;
    st = row.ValidateAgainst(schema);
    if (!st.ok()) return st;

    Value pk;
    st = row.PrimaryKey(schema, &pk);
    if (!st.ok()) return st;

    // Read-your-writes / duplicate in write set.
    const std::string wkey = WriteKey(table, pk);
    auto it = write_set_.find(wkey);
    if (it != write_set_.end()) {
        if (it->second.op == WriteOp::kDelete) {
            // Re-insert after delete in same txn → becomes update/insert of new row.
            it->second.op = WriteOp::kInsert;
            it->second.row = row;
            return lsmkv::Status::OK();
        }
        return lsmkv::Status::InvalidArgument("duplicate primary key in transaction");
    }

    // Optimistic check against snapshot (commit re-checks).
    Row existing;
    st = db_->store()->GetRow(table, pk, start_ts_, &existing);
    if (st.ok()) {
        return lsmkv::Status::InvalidArgument("duplicate primary key");
    }
    if (!st.IsNotFound()) return st;

    WriteEntry e;
    e.op = WriteOp::kInsert;
    e.table = table;
    e.pk = pk;
    e.row = row;
    write_set_.emplace(wkey, std::move(e));
    return lsmkv::Status::OK();
}

lsmkv::Status Transaction::Update(const std::string& table, const Row& row) {
    if (finished_) {
        return lsmkv::Status::InvalidArgument("transaction finished");
    }
    TableSchema schema;
    auto st = db_->catalog()->GetTable(table, &schema);
    if (!st.ok()) return st;
    st = row.ValidateAgainst(schema);
    if (!st.ok()) return st;

    Value pk;
    st = row.PrimaryKey(schema, &pk);
    if (!st.ok()) return st;

    const std::string wkey = WriteKey(table, pk);
    auto it = write_set_.find(wkey);
    if (it != write_set_.end()) {
        if (it->second.op == WriteOp::kDelete) {
            return lsmkv::Status::NotFound("row deleted in transaction");
        }
        // Insert or prior update — replace payload; keep op (insert stays insert).
        it->second.row = row;
        return lsmkv::Status::OK();
    }

    Row existing;
    st = db_->store()->GetRow(table, pk, start_ts_, &existing);
    if (st.IsNotFound()) {
        return lsmkv::Status::NotFound("row not found");
    }
    if (!st.ok()) return st;

    WriteEntry e;
    e.op = WriteOp::kUpdate;
    e.table = table;
    e.pk = pk;
    e.row = row;
    write_set_.emplace(wkey, std::move(e));
    return lsmkv::Status::OK();
}

lsmkv::Status Transaction::Delete(const std::string& table, const Value& pk) {
    if (finished_) {
        return lsmkv::Status::InvalidArgument("transaction finished");
    }
    TableSchema schema;
    auto st = db_->catalog()->GetTable(table, &schema);
    if (!st.ok()) return st;

    const std::string wkey = WriteKey(table, pk);
    auto it = write_set_.find(wkey);
    if (it != write_set_.end()) {
        if (it->second.op == WriteOp::kInsert) {
            // Insert then delete in same txn → remove from write set entirely.
            write_set_.erase(it);
            return lsmkv::Status::OK();
        }
        if (it->second.op == WriteOp::kDelete) {
            return lsmkv::Status::NotFound("row already deleted in transaction");
        }
        it->second.op = WriteOp::kDelete;
        it->second.row = Row();
        return lsmkv::Status::OK();
    }

    Row existing;
    st = db_->store()->GetRow(table, pk, start_ts_, &existing);
    if (st.IsNotFound()) {
        return lsmkv::Status::NotFound("row not found");
    }
    if (!st.ok()) return st;

    WriteEntry e;
    e.op = WriteOp::kDelete;
    e.table = table;
    e.pk = pk;
    write_set_.emplace(wkey, std::move(e));
    return lsmkv::Status::OK();
}

lsmkv::Status Transaction::Get(const std::string& table, const Value& pk, Row* out) {
    if (finished_) {
        return lsmkv::Status::InvalidArgument("transaction finished");
    }
    if (out == nullptr) {
        return lsmkv::Status::InvalidArgument("null out");
    }
    TableSchema schema;
    auto st = db_->catalog()->GetTable(table, &schema);
    if (!st.ok()) return st;

    // Read-your-writes.
    const std::string wkey = WriteKey(table, pk);
    auto it = write_set_.find(wkey);
    if (it != write_set_.end()) {
        if (it->second.op == WriteOp::kDelete) {
            return lsmkv::Status::NotFound("row deleted in transaction");
        }
        *out = it->second.row;
        return lsmkv::Status::OK();
    }

    return db_->store()->GetRow(table, pk, start_ts_, out);
}

lsmkv::Status Transaction::Commit() {
    if (finished_) {
        return lsmkv::Status::InvalidArgument("transaction finished");
    }
    return db_->CommitTransaction(this);
}

lsmkv::Status Transaction::Abort() {
    if (finished_) {
        return lsmkv::Status::OK();
    }
    write_set_.clear();
    db_->FinishTransaction(this);
    return lsmkv::Status::OK();
}

}  // namespace reldb
