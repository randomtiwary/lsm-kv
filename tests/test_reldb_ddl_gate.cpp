#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "test_harness.h"
#include "test_util.h"

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

reldb::TableSchema OtherSchema() {
    return reldb::TableSchema("other", {
        {"id", reldb::ColumnType::kInt64, true},
    });
}

std::shared_ptr<reldb::Database> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::shared_ptr<reldb::Database> db;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

}  // namespace

TEST(reldb_ddl_gate_create_blocked_by_open_txn) {
    auto dir = MakeTempDir("reldb_ddl_gate1");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        expect_eq(db->open_txn_count(), static_cast<std::size_t>(0), "count0");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        expect_eq(db->open_txn_count(), static_cast<std::size_t>(1), "count1");

        auto st = db->CreateTable(UsersSchema());
        expect(st.IsInvalidArgument(), "create blocked");
        expect(st.ToString().find("DDL requires no open transactions") != std::string::npos,
               "msg");

        EXPECT_OK(txn->Commit(), "commit");
        expect_eq(db->open_txn_count(), static_cast<std::size_t>(0), "count after commit");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create ok");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_ddl_gate_abort_and_dtor_release_count) {
    auto dir = MakeTempDir("reldb_ddl_gate2");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            expect_eq(db->open_txn_count(), static_cast<std::size_t>(1), "1");
            EXPECT_OK(txn->Abort(), "abort");
            expect_eq(db->open_txn_count(), static_cast<std::size_t>(0), "0 after abort");
        }

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin2");
            expect_eq(db->open_txn_count(), static_cast<std::size_t>(1), "1 again");
            // dtor Aborts
        }
        expect_eq(db->open_txn_count(), static_cast<std::size_t>(0), "0 after dtor");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_ddl_gate_conflict_commit_decrements) {
    auto dir = MakeTempDir("reldb_ddl_gate3");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        // Seed row.
        {
            std::unique_ptr<reldb::Transaction> seed;
            EXPECT_OK(db->Begin(&seed), "seed");
            EXPECT_OK(seed->Insert("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("a")})),
                      "ins");
            EXPECT_OK(seed->Commit(), "cseed");
        }
        expect_eq(db->open_txn_count(), static_cast<std::size_t>(0), "0");

        std::unique_ptr<reldb::Transaction> t1;
        std::unique_ptr<reldb::Transaction> t2;
        EXPECT_OK(db->Begin(&t1), "t1");
        EXPECT_OK(db->Begin(&t2), "t2");
        expect_eq(db->open_txn_count(), static_cast<std::size_t>(2), "2");

        EXPECT_OK(t1->Update("users", reldb::Row({reldb::Value::Int64(1),
                                                   reldb::Value::String("b")})),
                  "u1");
        // Early WW on t2.
        expect(t2->Update("users", reldb::Row({reldb::Value::Int64(1),
                                               reldb::Value::String("c")}))
                   .IsConflict(),
               "u2 conflict");
        EXPECT_OK(t1->Commit(), "c1");
        EXPECT_OK(t2->Abort(), "a2");
        expect_eq(db->open_txn_count(), static_cast<std::size_t>(0), "0 after finish");

        // DDL unblocked.
        EXPECT_OK(db->CreateTable(OtherSchema()), "other");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_ddl_gate_begin_blocked_when_ddl_flag) {
    auto dir = MakeTempDir("reldb_ddl_gate4");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        db->TEST_SetDdlInProgress(true);
        std::unique_ptr<reldb::Transaction> txn;
        auto st = db->Begin(&txn);
        expect(st.IsInvalidArgument(), "begin blocked");
        expect(st.ToString().find("DDL in progress") != std::string::npos, "msg");
        expect(txn == nullptr, "no txn");
        expect_eq(db->open_txn_count(), static_cast<std::size_t>(0), "count");
        db->TEST_SetDdlInProgress(false);
        EXPECT_OK(db->Begin(&txn), "begin ok");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_ddl_gate_abandon_releases_count) {
    auto dir = MakeTempDir("reldb_ddl_gate5");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        expect_eq(db->open_txn_count(), static_cast<std::size_t>(1), "1");
        txn->TEST_AbandonWithoutAbort();
        expect_eq(db->open_txn_count(), static_cast<std::size_t>(0), "0 after abandon");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_ddl_gate_create_blocks_while_other_holds_txn) {
    auto dir = MakeTempDir("reldb_ddl_gate6");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");

        std::unique_ptr<reldb::Transaction> holder;
        EXPECT_OK(db->Begin(&holder), "begin");

        std::atomic<bool> create_done{false};
        std::atomic<bool> create_ok{false};
        std::thread t([&]() {
            auto st = db->CreateTable(UsersSchema());
            create_ok.store(st.ok());
            create_done.store(true);
        });
        // CreateTable should fail quickly (not wait forever) while count > 0.
        for (int i = 0; i < 50 && !create_done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        expect(create_done.load(), "create returned");
        expect(!create_ok.load(), "create failed");
        EXPECT_OK(holder->Abort(), "abort");
        t.join();
        EXPECT_OK(db->CreateTable(UsersSchema()), "create after");
    }
    RemoveDirRecursive(dir);
}
