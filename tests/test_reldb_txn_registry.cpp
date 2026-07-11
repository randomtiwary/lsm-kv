#include <memory>

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

std::unique_ptr<reldb::Database> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::unique_ptr<reldb::Database> db;
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
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            expect(txn->txn_id() >= 1, "txn id");
            expect_eq(txn->start_ts(), static_cast<reldb::Timestamp>(0), "snap0");

            reldb::TxnMeta meta;
            EXPECT_OK(db->GetTxnMeta(txn->txn_id(), &meta), "meta");
            expect(meta.state == reldb::TxnState::kOpen, "open");

            reldb::Row got;
            expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "empty");
            EXPECT_OK(txn->Commit(), "commit empty");
            EXPECT_OK(db->GetTxnMeta(txn->txn_id(), &meta), "meta2");
            expect(meta.state == reldb::TxnState::kCommitted, "committed");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin2");
            expect(txn->start_ts() >= 1, "snap advanced");
            EXPECT_OK(txn->Abort(), "abort");
            reldb::TxnMeta meta;
            EXPECT_OK(db->GetTxnMeta(txn->txn_id(), &meta), "meta3");
            expect(meta.state == reldb::TxnState::kAborted, "aborted");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_registry_persists) {
    auto dir = MakeTempDir("reldb_reg2");
    reldb::TxnId id = 0;
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        id = txn->txn_id();
        EXPECT_OK(txn->Commit(), "commit");
    }
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "reopen");
        reldb::TxnMeta meta;
        EXPECT_OK(db->GetTxnMeta(id, &meta), "load");
        expect(meta.state == reldb::TxnState::kCommitted, "still committed");
    }
    RemoveDirRecursive(dir);
}
