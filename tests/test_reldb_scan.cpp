#include <memory>
#include <string>
#include <vector>

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

}  // namespace

TEST(reldb_scan_empty_table) {
    auto dir = MakeTempDir("reldb_scan_empty");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        std::unique_ptr<reldb::RowScan> scan;
        EXPECT_OK(txn->Scan("users", nullptr, nullptr, &scan), "scan");
        expect(!scan->Valid(), "empty");
        EXPECT_OK(scan->status(), "status");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_scan_full_table_order) {
    auto dir = MakeTempDir("reldb_scan_full");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            // Insert out of id order; scan order is encoded-PK / KV key order.
            EXPECT_OK(txn->Insert("users", User(3, "c")), "i3");
            EXPECT_OK(txn->Insert("users", User(1, "a")), "i1");
            EXPECT_OK(txn->Insert("users", User(2, "b")), "i2");
            EXPECT_OK(txn->Commit(), "commit");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin2");
            std::unique_ptr<reldb::RowScan> scan;
            EXPECT_OK(txn->Scan("users", nullptr, nullptr, &scan), "scan");
            std::vector<std::int64_t> ids;
            for (; scan->Valid(); scan->Next()) {
                ids.push_back(scan->pk().GetInt64());
            }
            EXPECT_OK(scan->status(), "status");
            expect_eq(static_cast<int>(ids.size()), 3, "count");
            // Encoded int64 hex order follows big-endian-ish encoding; for small
            // positive ids the order is ascending 1,2,3.
            expect_eq(ids[0], static_cast<std::int64_t>(1), "id0");
            expect_eq(ids[1], static_cast<std::int64_t>(2), "id1");
            expect_eq(ids[2], static_cast<std::int64_t>(3), "id2");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_scan_bounds_inclusive_start_exclusive_end) {
    auto dir = MakeTempDir("reldb_scan_bounds");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            for (std::int64_t i = 1; i <= 5; ++i) {
                EXPECT_OK(txn->Insert("users", User(i, "n")), "ins");
            }
            EXPECT_OK(txn->Commit(), "commit");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin2");
            const reldb::Value start = reldb::Value::Int64(2);
            const reldb::Value end = reldb::Value::Int64(5);  // exclusive
            std::unique_ptr<reldb::RowScan> scan;
            EXPECT_OK(txn->Scan("users", &start, &end, &scan), "scan");
            std::vector<std::int64_t> ids;
            for (; scan->Valid(); scan->Next()) {
                ids.push_back(scan->pk().GetInt64());
            }
            expect_eq(static_cast<int>(ids.size()), 3, "2,3,4");
            expect_eq(ids[0], static_cast<std::int64_t>(2), "2");
            expect_eq(ids[1], static_cast<std::int64_t>(3), "3");
            expect_eq(ids[2], static_cast<std::int64_t>(4), "4");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_scan_read_your_writes_and_skip_deleted) {
    auto dir = MakeTempDir("reldb_scan_ryw");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "seed");
            EXPECT_OK(txn->Insert("users", User(1, "a")), "i1");
            EXPECT_OK(txn->Insert("users", User(2, "b")), "i2");
            EXPECT_OK(txn->Commit(), "c");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            EXPECT_OK(txn->Insert("users", User(3, "c")), "i3 provisional");
            EXPECT_OK(txn->Delete("users", reldb::Value::Int64(1)), "del 1");
            std::vector<std::int64_t> ids;
            {
                std::unique_ptr<reldb::RowScan> scan;
                EXPECT_OK(txn->Scan("users", nullptr, nullptr, &scan), "scan");
                for (; scan->Valid(); scan->Next()) {
                    ids.push_back(scan->pk().GetInt64());
                }
                EXPECT_OK(scan->status(), "status");
            }
            // 1 deleted, 2 committed, 3 own provisional
            expect_eq(static_cast<int>(ids.size()), 2, "two rows");
            expect_eq(ids[0], static_cast<std::int64_t>(2), "2");
            expect_eq(ids[1], static_cast<std::int64_t>(3), "3");
            EXPECT_OK(txn->Abort(), "abort");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_scan_snapshot_isolation) {
    auto dir = MakeTempDir("reldb_scan_si");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "seed");
            EXPECT_OK(txn->Insert("users", User(1, "old")), "i");
            EXPECT_OK(txn->Commit(), "c");
        }
        std::unique_ptr<reldb::Transaction> t1;
        EXPECT_OK(db->Begin(&t1), "t1");
        {
            std::unique_ptr<reldb::Transaction> t2;
            EXPECT_OK(db->Begin(&t2), "t2");
            EXPECT_OK(t2->Insert("users", User(2, "new")), "i2");
            EXPECT_OK(t2->Update("users", User(1, "upd")), "u1");
            EXPECT_OK(t2->Commit(), "c2");
        }
        std::vector<std::int64_t> ids;
        std::string name1;
        {
            std::unique_ptr<reldb::RowScan> scan;
            EXPECT_OK(t1->Scan("users", nullptr, nullptr, &scan), "scan");
            for (; scan->Valid(); scan->Next()) {
                ids.push_back(scan->pk().GetInt64());
                if (scan->pk().GetInt64() == 1) {
                    name1 = scan->row().at(1).GetString();
                }
            }
            EXPECT_OK(scan->status(), "status");
        }
        expect_eq(static_cast<int>(ids.size()), 1, "only pre-snapshot row");
        expect_eq(ids[0], static_cast<std::int64_t>(1), "id1");
        expect_eq(name1, std::string("old"), "snapshot value");
        EXPECT_OK(t1->Abort(), "abort t1");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_scan_rejects_finished_and_nonempty_out) {
    auto dir = MakeTempDir("reldb_scan_err");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            EXPECT_OK(txn->Commit(), "commit");
            std::unique_ptr<reldb::RowScan> scan;
            expect(txn->Scan("users", nullptr, nullptr, &scan).IsInvalidArgument(), "finished");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin2");
            std::unique_ptr<reldb::RowScan> scan;
            EXPECT_OK(txn->Scan("users", nullptr, nullptr, &scan), "scan1");
            expect(txn->Scan("users", nullptr, nullptr, &scan).IsInvalidArgument(), "nonempty out");
        }
    }
    RemoveDirRecursive(dir);
}
