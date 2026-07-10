#include "reldb/txn.h"

#include "reldb/database.h"
#include "reldb/macros.h"

namespace reldb {

Transaction::Transaction(Database* db, TxnId txn_id, Timestamp start_ts)
    : db_(db), txn_id_(txn_id), start_ts_(start_ts) {}

Transaction::~Transaction() {
    if (!finished_) Abort();
}

lsmkv::Status Transaction::Get(const std::string& table, const Value& pk, Row* out) {
    if (finished_) return STATUS(InvalidArgument, "transaction finished");
    if (out == nullptr) return STATUS(InvalidArgument, "null out");
    TableSchema schema;
    RELDB_RETURN_NOT_OK(db_->catalog()->GetTable(table, &schema));
    std::lock_guard<std::mutex> lock(db_->mu_);
    return db_->store_->GetRow(
        table, pk, start_ts_, txn_id_,
        [this](TxnId id, TxnMeta* m) { return db_->GetTxnMeta(id, m); }, out);
}

lsmkv::Status Transaction::Commit() {
    if (finished_) return STATUS(InvalidArgument, "transaction finished");
    return db_->CommitTransaction(this);
}

lsmkv::Status Transaction::Abort() {
    if (finished_) return STATUS(OK);
    return db_->AbortTransaction(this);
}

}  // namespace reldb
