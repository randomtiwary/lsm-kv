#include <memory>

#include "test_harness.h"
#include "test_util.h"

#include "lsmkv/status.h"
#include "reldb/database.h"
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

std::shared_ptr<reldb::Database> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::shared_ptr<reldb::Database> db;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

}  // namespace

TEST(reldb_txn_insert_get_commit) {
    auto dir = MakeTempDir("reldb_txn1");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            expect_eq(txn->start_ts(), static_cast<reldb::Timestamp>(0), "first snap 0");
            EXPECT_OK(txn->Insert("users", User(1, "ann")), "insert");
            reldb::Row got;
            EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &got), "ryw");
            expect(got.at(1) == reldb::Value::String("ann"), "ann");
            EXPECT_OK(txn->Commit(), "commit");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin2");
            expect(txn->start_ts() >= 1, "snap advanced");
            reldb::Row got;
            EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &got), "get committed");
            expect(got.at(1) == reldb::Value::String("ann"), "ann2");
            EXPECT_OK(txn->Commit(), "commit empty");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_abort_discards_writes) {
    auto dir = MakeTempDir("reldb_txn2");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            EXPECT_OK(txn->Insert("users", User(1, "ghost")), "insert");
            EXPECT_OK(txn->Abort(), "abort");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin2");
            reldb::Row got;
            expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "gone");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_update_and_delete) {
    auto dir = MakeTempDir("reldb_txn3");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b1");
            EXPECT_OK(txn->Insert("users", User(1, "ann")), "ins");
            EXPECT_OK(txn->Commit(), "c1");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b2");
            EXPECT_OK(txn->Update("users", User(1, "bob")), "upd");
            reldb::Row got;
            EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &got), "ryw upd");
            expect(got.at(1) == reldb::Value::String("bob"), "bob");
            EXPECT_OK(txn->Commit(), "c2");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b3");
            reldb::Row got;
            EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &got), "get bob");
            expect(got.at(1) == reldb::Value::String("bob"), "bob2");
            EXPECT_OK(txn->Delete("users", reldb::Value::Int64(1)), "del");
            expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "ryw del");
            EXPECT_OK(txn->Commit(), "c3");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b4");
            reldb::Row got;
            expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "deleted");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_snapshot_isolation_read) {
    // T1 begins, T2 commits a change, T1 still sees the old snapshot.
    auto dir = MakeTempDir("reldb_txn4");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> t_setup;
            EXPECT_OK(db->Begin(&t_setup), "setup");
            EXPECT_OK(t_setup->Insert("users", User(1, "ann")), "ins");
            EXPECT_OK(t_setup->Commit(), "csetup");
        }

        std::unique_ptr<reldb::Transaction> t1;
        EXPECT_OK(db->Begin(&t1), "t1 begin");
        reldb::Row got;
        EXPECT_OK(t1->Get("users", reldb::Value::Int64(1), &got), "t1 see ann");
        expect(got.at(1) == reldb::Value::String("ann"), "ann");

        {
            std::unique_ptr<reldb::Transaction> t2;
            EXPECT_OK(db->Begin(&t2), "t2 begin");
            EXPECT_OK(t2->Update("users", User(1, "bob")), "t2 upd");
            EXPECT_OK(t2->Commit(), "t2 commit");
        }

        // T1 snapshot must not observe T2's commit.
        EXPECT_OK(t1->Get("users", reldb::Value::Int64(1), &got), "t1 still");
        expect(got.at(1) == reldb::Value::String("ann"), "still ann");
        EXPECT_OK(t1->Commit(), "t1 commit");

        std::unique_ptr<reldb::Transaction> t3;
        EXPECT_OK(db->Begin(&t3), "t3");
        EXPECT_OK(t3->Get("users", reldb::Value::Int64(1), &got), "t3");
        expect(got.at(1) == reldb::Value::String("bob"), "now bob");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_write_write_conflict) {
    auto dir = MakeTempDir("reldb_txn5");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> t0;
            EXPECT_OK(db->Begin(&t0), "seed");
            EXPECT_OK(t0->Insert("users", User(1, "ann")), "ins");
            EXPECT_OK(t0->Commit(), "cseed");
        }

        std::unique_ptr<reldb::Transaction> t1;
        std::unique_ptr<reldb::Transaction> t2;
        EXPECT_OK(db->Begin(&t1), "t1");
        EXPECT_OK(db->Begin(&t2), "t2");
        EXPECT_OK(t1->Update("users", User(1, "from1")), "u1");
        // Early WW: second open writer on the same PK fails on write, not commit.
        expect(t2->Update("users", User(1, "from2")).IsConflict(), "u2");

        EXPECT_OK(t1->Commit(), "t1 wins");
        EXPECT_OK(t2->Abort(), "t2 abort");

        std::unique_ptr<reldb::Transaction> t3;
        EXPECT_OK(db->Begin(&t3), "t3");
        reldb::Row got;
        EXPECT_OK(t3->Get("users", reldb::Value::Int64(1), &got), "get");
        expect(got.at(1) == reldb::Value::String("from1"), "winner");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_duplicate_pk) {
    auto dir = MakeTempDir("reldb_txn6");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b1");
            EXPECT_OK(txn->Insert("users", User(1, "a")), "i1");
            expect(txn->Insert("users", User(1, "b")).IsInvalidArgument(), "dup ws");
            EXPECT_OK(txn->Commit(), "c1");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b2");
            expect(txn->Insert("users", User(1, "c")).IsInvalidArgument(), "dup committed");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_persists_across_reopen) {
    auto dir = MakeTempDir("reldb_txn7");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "b");
        EXPECT_OK(txn->Insert("users", User(42, "persist")), "i");
        EXPECT_OK(txn->Commit(), "c");
    }
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "reopen");
        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "b");
        reldb::Row got;
        EXPECT_OK(txn->Get("users", reldb::Value::Int64(42), &got), "get");
        expect(got.at(1) == reldb::Value::String("persist"), "val");
        // Oracle advanced: new writes get fresh timestamps.
        EXPECT_OK(txn->Insert("users", User(43, "next")), "i2");
        EXPECT_OK(txn->Commit(), "c2");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_insert_then_delete_same_txn) {
    auto dir = MakeTempDir("reldb_txn8");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b");
            EXPECT_OK(txn->Insert("users", User(1, "tmp")), "i");
            EXPECT_OK(txn->Delete("users", reldb::Value::Int64(1)), "d");
            reldb::Row got;
            expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "gone");
            EXPECT_OK(txn->Commit(), "c");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b2");
            reldb::Row got;
            expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "still gone");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_early_write_write_conflict) {
    auto dir = MakeTempDir("reldb_txn_eww");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> t1;
            std::unique_ptr<reldb::Transaction> t2;
            EXPECT_OK(db->Begin(&t1), "t1");
            EXPECT_OK(db->Begin(&t2), "t2");
            EXPECT_OK(t1->Insert("users", User(1, "a")), "t1 ins");
            // t2 tries same PK while t1 still open => early WW conflict
            expect(t2->Insert("users", User(1, "b")).IsConflict(), "t2 conflict");
            EXPECT_OK(t1->Commit(), "t1 commit");
            // t2 still owns the failed open txn until scope ends
            EXPECT_OK(t2->Abort(), "t2 abort");
        }
        // After t1 commits, a new txn can proceed
        {
            std::unique_ptr<reldb::Transaction> t2;
            EXPECT_OK(db->Begin(&t2), "t2b");
            EXPECT_OK(t2->Update("users", User(1, "c")), "t2 upd");
            EXPECT_OK(t2->Commit(), "t2 commit");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_eager_write_visible_only_after_commit) {
    auto dir = MakeTempDir("reldb_txn_eager");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        std::unique_ptr<reldb::Transaction> t1;
        EXPECT_OK(db->Begin(&t1), "t1");
        EXPECT_OK(t1->Insert("users", User(1, "secret")), "ins");

        {
            std::unique_ptr<reldb::Transaction> t2;
            EXPECT_OK(db->Begin(&t2), "t2");
            reldb::Row got;
            // t2 must not see t1's provisional write
            expect(t2->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "t2 no dirty read");
            EXPECT_OK(t1->Commit(), "t1 commit");
            // t2 still on old snapshot
            expect(t2->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "t2 snap");
        }
        {
            std::unique_ptr<reldb::Transaction> t3;
            EXPECT_OK(db->Begin(&t3), "t3");
            reldb::Row got;
            EXPECT_OK(t3->Get("users", reldb::Value::Int64(1), &got), "see committed");
            expect(got.at(1) == reldb::Value::String("secret"), "val");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_begin_rejects_non_empty_unique_ptr) {
    auto dir = MakeTempDir("reldb_txn_begin");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        expect(db->Begin(&txn).IsInvalidArgument(), "reuse rejected");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}
