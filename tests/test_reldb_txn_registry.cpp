#include "test_harness.h"
#include "test_util.h"

#include "reldb/database.h"
#include "reldb/schema.h"
#include "reldb/txn.h"
#include "reldb/types.h"

namespace {

reldb::TableSchema UsersSchema() {
    return reldb::TableSchema("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
    });
}

reldb::Database* OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    reldb::Database* db = nullptr;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

}  // namespace

TEST(reldb_status_conflict) {
    auto s = lsmkv::Status::Conflict("ww");
    expect(s.IsConflict(), "is conflict");
    expect(s.ToString().find("Conflict") != std::string::npos, "str");
}

TEST(reldb_txn_begin_commit_abort_registry) {
    auto dir = MakeTempDir("reldb_reg1");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* txn = nullptr;
    expect(db->Begin(&txn).ok(), "begin");
    expect(txn->txn_id() >= 1, "txn id");
    expect_eq(txn->start_ts(), static_cast<reldb::Timestamp>(0), "snap0");

    reldb::TxnMeta meta;
    expect(db->GetTxnMeta(txn->txn_id(), &meta).ok(), "meta");
    expect(meta.state == reldb::TxnState::kOpen, "open");

    reldb::Row got;
    expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "empty");
    expect(txn->Commit().ok(), "commit empty");
    expect(db->GetTxnMeta(txn->txn_id(), &meta).ok(), "meta2");
    expect(meta.state == reldb::TxnState::kCommitted, "committed");
    delete txn;

    expect(db->Begin(&txn).ok(), "begin2");
    expect(txn->start_ts() >= 1, "snap advanced");
    expect(txn->Abort().ok(), "abort");
    expect(db->GetTxnMeta(txn->txn_id(), &meta).ok(), "meta3");
    expect(meta.state == reldb::TxnState::kAborted, "aborted");
    delete txn;
    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_registry_persists) {
    auto dir = MakeTempDir("reldb_reg2");
    reldb::TxnId id = 0;
    {
        reldb::Database* db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::Transaction* txn = nullptr;
        expect(db->Begin(&txn).ok(), "begin");
        id = txn->txn_id();
        expect(txn->Commit().ok(), "commit");
        delete txn;
        delete db;
    }
    {
        reldb::Database* db = OpenDb(dir);
        expect(db != nullptr, "reopen");
        reldb::TxnMeta meta;
        expect(db->GetTxnMeta(id, &meta).ok(), "load");
        expect(meta.state == reldb::TxnState::kCommitted, "still committed");
        delete db;
    }
    RemoveDirRecursive(dir);
}
