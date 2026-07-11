#include <memory>

#include "test_harness.h"
#include "test_util.h"

#include "reldb/database.h"
#include "reldb/mvcc.h"
#include "reldb/row.h"
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

reldb::Row User(std::int64_t id, const std::string& name) {
    return reldb::Row({reldb::Value::Int64(id), reldb::Value::String(name)});
}

std::unique_ptr<reldb::Database> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::unique_ptr<reldb::Database> db;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

// Simulate process crash: forget the in-memory Transaction without Abort/Commit,
// then drop the Database. Uses release() intentionally (no RAII cleanup).
void SimulateCrashDrop(std::unique_ptr<reldb::Transaction>& txn,
                       std::unique_ptr<reldb::Database>& db) {
    (void)txn.release();  // leak: crash would not run ~Transaction
    db.reset();
}

}  // namespace

// Crash with only Open provisionals: reopen aborts the txn and restores heads.
TEST(reldb_recovery_open_txn_aborted_on_reopen) {
    auto dir = MakeTempDir("reldb_rec1");
    reldb::TxnId id = 0;
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        id = txn->txn_id();
        EXPECT_OK(txn->Insert("users", User(1, "ghost")), "insert provisional");
        SimulateCrashDrop(txn, db);
    }

    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "reopen");

        reldb::TxnMeta meta;
        EXPECT_OK(db->GetTxnMeta(id, &meta), "meta");
        expect(meta.state == reldb::TxnState::kAborted, "open became aborted");

        std::unique_ptr<reldb::Transaction> check;
        EXPECT_OK(db->Begin(&check), "begin check");
        reldb::Row row;
        expect(check->Get("users", reldb::Value::Int64(1), &row).IsNotFound(), "ghost gone");
        EXPECT_OK(check->Insert("users", User(1, "alive")), "reinsert");
        EXPECT_OK(check->Commit(), "commit");
    }
    RemoveDirRecursive(dir);
}

// Simulate crash mid-commit: Committing intent present, versions still provisional.
TEST(reldb_recovery_committing_intent_redo) {
    auto dir = MakeTempDir("reldb_rec2");
    reldb::TxnId id = 0;
    reldb::TxnWrite written;
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        id = txn->txn_id();
        EXPECT_OK(txn->Insert("users", User(7, "partial")), "insert");

        reldb::TxnMeta open_meta;
        EXPECT_OK(db->GetTxnMeta(id, &open_meta), "open meta");
        expect(!open_meta.writes.empty(), "writes durable");
        written = open_meta.writes[0];

        reldb::TxnMeta intent;
        intent.state = reldb::TxnState::kCommitting;
        intent.commit_ts = 100;
        intent.writes = open_meta.writes;
        EXPECT_OK(db->PutTxnMeta(id, intent), "forge committing");

        SimulateCrashDrop(txn, db);
    }

    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "reopen");

        reldb::TxnMeta meta;
        EXPECT_OK(db->GetTxnMeta(id, &meta), "meta after recover");
        expect(meta.state == reldb::TxnState::kCommitted, "committing became committed");
        expect_eq(meta.commit_ts, static_cast<reldb::Timestamp>(100), "commit_ts kept");

        std::unique_ptr<reldb::Transaction> check;
        EXPECT_OK(db->Begin(&check), "begin check");
        reldb::Row row;
        EXPECT_OK(check->Get("users", reldb::Value::Int64(7), &row), "row visible");
        expect_eq(row.at(1).GetString(), std::string("partial"), "payload");
        EXPECT_OK(check->Abort(), "abort check");

        reldb::VersionRecord rec;
        EXPECT_OK(db->store()->GetVersion("users", reldb::Value::Int64(7),
                                          written.version_id, &rec),
                  "get version");
        expect(!rec.is_provisional(), "stamped");
        expect_eq(rec.start_ts, static_cast<reldb::Timestamp>(100), "start_ts");
    }
    RemoveDirRecursive(dir);
}

// Partial apply: some versions already stamped, intent still Committing → redo is idempotent.
TEST(reldb_recovery_committing_partial_apply_idempotent) {
    auto dir = MakeTempDir("reldb_rec3");
    reldb::TxnId id = 0;
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        id = txn->txn_id();
        EXPECT_OK(txn->Insert("users", User(1, "a")), "i1");
        EXPECT_OK(txn->Insert("users", User(2, "b")), "i2");

        reldb::TxnMeta open_meta;
        EXPECT_OK(db->GetTxnMeta(id, &open_meta), "meta");
        expect_eq(static_cast<int>(open_meta.writes.size()), 2, "two writes");

        const reldb::Timestamp commit_ts = 50;
        {
            const auto& w0 = open_meta.writes[0];
            reldb::VersionRecord rec;
            EXPECT_OK(db->store()->GetVersion(w0.table, w0.pk, w0.version_id, &rec), "v0");
            rec.start_ts = commit_ts;
            EXPECT_OK(db->store()->PutVersionValue(w0.table, w0.pk, rec), "stamp0");
        }

        reldb::TxnMeta intent;
        intent.state = reldb::TxnState::kCommitting;
        intent.commit_ts = commit_ts;
        intent.writes = open_meta.writes;
        EXPECT_OK(db->PutTxnMeta(id, intent), "intent");
        SimulateCrashDrop(txn, db);
    }

    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "reopen");

        reldb::TxnMeta meta;
        EXPECT_OK(db->GetTxnMeta(id, &meta), "meta");
        expect(meta.state == reldb::TxnState::kCommitted, "committed");

        std::unique_ptr<reldb::Transaction> check;
        EXPECT_OK(db->Begin(&check), "check");
        reldb::Row r1, r2;
        EXPECT_OK(check->Get("users", reldb::Value::Int64(1), &r1), "g1");
        EXPECT_OK(check->Get("users", reldb::Value::Int64(2), &r2), "g2");
        expect_eq(r1.at(1).GetString(), std::string("a"), "a");
        expect_eq(r2.at(1).GetString(), std::string("b"), "b");
        EXPECT_OK(check->Abort(), "abort check");
    }
    RemoveDirRecursive(dir);
}

// Happy path still works with Option A (intent → apply → committed).
TEST(reldb_recovery_normal_commit_still_works) {
    auto dir = MakeTempDir("reldb_rec4");
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        EXPECT_OK(txn->Insert("users", User(1, "ok")), "ins");
        EXPECT_OK(txn->Commit(), "commit");
    }
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "reopen");
        std::unique_ptr<reldb::Transaction> check;
        EXPECT_OK(db->Begin(&check), "begin");
        reldb::Row row;
        EXPECT_OK(check->Get("users", reldb::Value::Int64(1), &row), "get");
        expect_eq(row.at(1).GetString(), std::string("ok"), "name");
        EXPECT_OK(check->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}
