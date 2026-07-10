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

reldb::Database* OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    reldb::Database* db = nullptr;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

}  // namespace

TEST(reldb_txn_insert_get_commit) {
    auto dir = MakeTempDir("reldb_txn1");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* txn = nullptr;
    expect(db->Begin(&txn).ok(), "begin");
    expect_eq(txn->start_ts(), static_cast<reldb::Timestamp>(0), "first snap 0");
    expect(txn->Insert("users", User(1, "ann")).ok(), "insert");
    reldb::Row got;
    expect(txn->Get("users", reldb::Value::Int64(1), &got).ok(), "ryw");
    expect(got.at(1) == reldb::Value::String("ann"), "ann");
    expect(txn->Commit().ok(), "commit");
    delete txn;

    expect(db->Begin(&txn).ok(), "begin2");
    expect(txn->start_ts() >= 1, "snap advanced");
    expect(txn->Get("users", reldb::Value::Int64(1), &got).ok(), "get committed");
    expect(got.at(1) == reldb::Value::String("ann"), "ann2");
    expect(txn->Commit().ok(), "commit empty");
    delete txn;

    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_abort_discards_writes) {
    auto dir = MakeTempDir("reldb_txn2");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* txn = nullptr;
    expect(db->Begin(&txn).ok(), "begin");
    expect(txn->Insert("users", User(1, "ghost")).ok(), "insert");
    expect(txn->Abort().ok(), "abort");
    delete txn;

    expect(db->Begin(&txn).ok(), "begin2");
    reldb::Row got;
    expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "gone");
    delete txn;
    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_update_and_delete) {
    auto dir = MakeTempDir("reldb_txn3");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* txn = nullptr;
    expect(db->Begin(&txn).ok(), "b1");
    expect(txn->Insert("users", User(1, "ann")).ok(), "ins");
    expect(txn->Commit().ok(), "c1");
    delete txn;

    expect(db->Begin(&txn).ok(), "b2");
    expect(txn->Update("users", User(1, "bob")).ok(), "upd");
    reldb::Row got;
    expect(txn->Get("users", reldb::Value::Int64(1), &got).ok(), "ryw upd");
    expect(got.at(1) == reldb::Value::String("bob"), "bob");
    expect(txn->Commit().ok(), "c2");
    delete txn;

    expect(db->Begin(&txn).ok(), "b3");
    expect(txn->Get("users", reldb::Value::Int64(1), &got).ok(), "get bob");
    expect(got.at(1) == reldb::Value::String("bob"), "bob2");
    expect(txn->Delete("users", reldb::Value::Int64(1)).ok(), "del");
    expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "ryw del");
    expect(txn->Commit().ok(), "c3");
    delete txn;

    expect(db->Begin(&txn).ok(), "b4");
    expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "deleted");
    delete txn;
    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_snapshot_isolation_read) {
    // T1 begins, T2 commits a change, T1 still sees the old snapshot.
    auto dir = MakeTempDir("reldb_txn4");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* t_setup = nullptr;
    expect(db->Begin(&t_setup).ok(), "setup");
    expect(t_setup->Insert("users", User(1, "ann")).ok(), "ins");
    expect(t_setup->Commit().ok(), "csetup");
    delete t_setup;

    reldb::Transaction* t1 = nullptr;
    expect(db->Begin(&t1).ok(), "t1 begin");
    reldb::Row got;
    expect(t1->Get("users", reldb::Value::Int64(1), &got).ok(), "t1 see ann");
    expect(got.at(1) == reldb::Value::String("ann"), "ann");

    reldb::Transaction* t2 = nullptr;
    expect(db->Begin(&t2).ok(), "t2 begin");
    expect(t2->Update("users", User(1, "bob")).ok(), "t2 upd");
    expect(t2->Commit().ok(), "t2 commit");
    delete t2;

    // T1 snapshot must not observe T2's commit.
    expect(t1->Get("users", reldb::Value::Int64(1), &got).ok(), "t1 still");
    expect(got.at(1) == reldb::Value::String("ann"), "still ann");
    expect(t1->Commit().ok(), "t1 commit");
    delete t1;

    reldb::Transaction* t3 = nullptr;
    expect(db->Begin(&t3).ok(), "t3");
    expect(t3->Get("users", reldb::Value::Int64(1), &got).ok(), "t3");
    expect(got.at(1) == reldb::Value::String("bob"), "now bob");
    delete t3;

    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_write_write_conflict) {
    auto dir = MakeTempDir("reldb_txn5");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* t0 = nullptr;
    expect(db->Begin(&t0).ok(), "seed");
    expect(t0->Insert("users", User(1, "ann")).ok(), "ins");
    expect(t0->Commit().ok(), "cseed");
    delete t0;

    reldb::Transaction* t1 = nullptr;
    reldb::Transaction* t2 = nullptr;
    expect(db->Begin(&t1).ok(), "t1");
    expect(db->Begin(&t2).ok(), "t2");
    expect(t1->Update("users", User(1, "from1")).ok(), "u1");
    // Early WW: second open writer on the same PK fails on write, not commit.
    expect(t2->Update("users", User(1, "from2")).IsConflict(), "u2");

    expect(t1->Commit().ok(), "t1 wins");
    expect(t2->Abort().ok(), "t2 abort");
    delete t1;
    delete t2;

    reldb::Transaction* t3 = nullptr;
    expect(db->Begin(&t3).ok(), "t3");
    reldb::Row got;
    expect(t3->Get("users", reldb::Value::Int64(1), &got).ok(), "get");
    expect(got.at(1) == reldb::Value::String("from1"), "winner");
    delete t3;

    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_duplicate_pk) {
    auto dir = MakeTempDir("reldb_txn6");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* txn = nullptr;
    expect(db->Begin(&txn).ok(), "b1");
    expect(txn->Insert("users", User(1, "a")).ok(), "i1");
    expect(txn->Insert("users", User(1, "b")).IsInvalidArgument(), "dup ws");
    expect(txn->Commit().ok(), "c1");
    delete txn;

    expect(db->Begin(&txn).ok(), "b2");
    expect(txn->Insert("users", User(1, "c")).IsInvalidArgument(), "dup committed");
    delete txn;

    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_persists_across_reopen) {
    auto dir = MakeTempDir("reldb_txn7");
    {
        reldb::Database* db = OpenDb(dir);
        expect(db != nullptr, "open");
        expect(db->CreateTable(UsersSchema()).ok(), "create");
        reldb::Transaction* txn = nullptr;
        expect(db->Begin(&txn).ok(), "b");
        expect(txn->Insert("users", User(42, "persist")).ok(), "i");
        expect(txn->Commit().ok(), "c");
        delete txn;
        delete db;
    }
    {
        reldb::Database* db = OpenDb(dir);
        expect(db != nullptr, "reopen");
        reldb::Transaction* txn = nullptr;
        expect(db->Begin(&txn).ok(), "b");
        reldb::Row got;
        expect(txn->Get("users", reldb::Value::Int64(42), &got).ok(), "get");
        expect(got.at(1) == reldb::Value::String("persist"), "val");
        // Oracle advanced: new writes get fresh timestamps.
        expect(txn->Insert("users", User(43, "next")).ok(), "i2");
        expect(txn->Commit().ok(), "c2");
        delete txn;
        delete db;
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_insert_then_delete_same_txn) {
    auto dir = MakeTempDir("reldb_txn8");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* txn = nullptr;
    expect(db->Begin(&txn).ok(), "b");
    expect(txn->Insert("users", User(1, "tmp")).ok(), "i");
    expect(txn->Delete("users", reldb::Value::Int64(1)).ok(), "d");
    reldb::Row got;
    expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "gone");
    expect(txn->Commit().ok(), "c");
    delete txn;

    expect(db->Begin(&txn).ok(), "b2");
    expect(txn->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "still gone");
    delete txn;
    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_early_write_write_conflict) {
    auto dir = MakeTempDir("reldb_txn_eww");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* t1 = nullptr;
    reldb::Transaction* t2 = nullptr;
    expect(db->Begin(&t1).ok(), "t1");
    expect(db->Begin(&t2).ok(), "t2");
    expect(t1->Insert("users", User(1, "a")).ok(), "t1 ins");
    // t2 tries same PK while t1 still open => early WW conflict
    expect(t2->Insert("users", User(1, "b")).IsConflict(), "t2 conflict");
    expect(t1->Commit().ok(), "t1 commit");
    delete t1;
    delete t2;

    // After t1 commits, a new txn can proceed
    expect(db->Begin(&t2).ok(), "t2b");
    expect(t2->Update("users", User(1, "c")).ok(), "t2 upd");
    expect(t2->Commit().ok(), "t2 commit");
    delete t2;
    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_txn_eager_write_visible_only_after_commit) {
    auto dir = MakeTempDir("reldb_txn_eager");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* t1 = nullptr;
    reldb::Transaction* t2 = nullptr;
    expect(db->Begin(&t1).ok(), "t1");
    expect(t1->Insert("users", User(1, "secret")).ok(), "ins");

    expect(db->Begin(&t2).ok(), "t2");
    reldb::Row got;
    // t2 must not see t1's provisional write
    expect(t2->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "t2 no dirty read");
    expect(t1->Commit().ok(), "t1 commit");
    delete t1;

    // t2 still on old snapshot
    expect(t2->Get("users", reldb::Value::Int64(1), &got).IsNotFound(), "t2 snap");
    delete t2;

    expect(db->Begin(&t2).ok(), "t3");
    expect(t2->Get("users", reldb::Value::Int64(1), &got).ok(), "see committed");
    expect(got.at(1) == reldb::Value::String("secret"), "val");
    delete t2;
    delete db;
    RemoveDirRecursive(dir);
}
