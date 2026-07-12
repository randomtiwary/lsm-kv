#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_harness.h"
#include "test_util.h"

#include "reldb/database.h"
#include "reldb/row.h"
#include "reldb/schema.h"
#include "reldb/txn.h"
#include "reldb/types.h"

namespace {

std::shared_ptr<reldb::Database> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::shared_ptr<reldb::Database> db;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

reldb::TableSchema TableN(int n) {
    return reldb::TableSchema("t" + std::to_string(n), {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
    });
}

}  // namespace

// Concurrent CreateTable (distinct names) + GetTable / HasTable must not crash
// or corrupt the catalog cache (A0.5: Database shared_mutex: shared on cache hit, exclusive on create/load..
TEST(reldb_catalog_mt_create_and_get) {
    auto dir = MakeTempDir("reldb_cat_mt");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");

        constexpr int kTables = 32;
        constexpr int kReaders = 4;
        std::atomic<int> creates_ok{0};
        std::atomic<int> gets_ok{0};
        std::atomic<int> errors{0};

        std::vector<std::thread> threads;

        // Creators: one table each.
        for (int i = 0; i < kTables; ++i) {
            threads.emplace_back([&, i]() {
                auto st = db->CreateTable(TableN(i));
                if (st.ok()) {
                    creates_ok.fetch_add(1);
                } else {
                    errors.fetch_add(1);
                }
            });
        }

        // Readers: hammer GetTable / HasTable while creates race.
        for (int r = 0; r < kReaders; ++r) {
            threads.emplace_back([&]() {
                for (int round = 0; round < 200; ++round) {
                    for (int i = 0; i < kTables; ++i) {
                        const std::string name = "t" + std::to_string(i);
                        reldb::TableSchema got;
                        auto st = db->GetTable(name, &got);
                        if (st.ok()) {
                            if (got.name() != name || got.num_columns() != 2) {
                                errors.fetch_add(1);
                            } else {
                                gets_ok.fetch_add(1);
                            }
                        } else if (!st.IsNotFound()) {
                            errors.fetch_add(1);
                        }
                        bool exists = false;
                        auto hs = db->HasTable(name, &exists);
                        if (!hs.ok()) {
                            errors.fetch_add(1);
                        } else if (exists) {
                            // Must be gettable if HasTable says true.
                            reldb::TableSchema again;
                            if (!db->GetTable(name, &again).ok()) {
                                errors.fetch_add(1);
                            }
                        }
                    }
                }
            });
        }

        for (auto& th : threads) th.join();

        expect_eq(creates_ok.load(), kTables, "all creates ok");
        expect_eq(errors.load(), 0, "no catalog errors");
        expect(gets_ok.load() > 0, "some gets succeeded");

        // After quiescence every table is visible.
        for (int i = 0; i < kTables; ++i) {
            reldb::TableSchema got;
            EXPECT_OK(db->GetTable("t" + std::to_string(i), &got), "final get");
            expect_eq(got.name(), std::string("t") + std::to_string(i), "name");
        }
    }
    RemoveDirRecursive(dir);
}

// Same-name CreateTable from many threads: exactly one OK, rest InvalidArgument.
TEST(reldb_catalog_mt_duplicate_create) {
    auto dir = MakeTempDir("reldb_cat_mt_dup");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");

        constexpr int kThreads = 8;
        std::atomic<int> ok_count{0};
        std::atomic<int> dup_count{0};
        std::atomic<int> other{0};

        const reldb::TableSchema schema("users", {
            {"id", reldb::ColumnType::kInt64, true},
            {"name", reldb::ColumnType::kString, false},
        });

        std::vector<std::thread> threads;
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&]() {
                auto st = db->CreateTable(schema);
                if (st.ok()) {
                    ok_count.fetch_add(1);
                } else if (st.IsInvalidArgument()) {
                    dup_count.fetch_add(1);
                } else {
                    other.fetch_add(1);
                }
            });
        }
        for (auto& th : threads) th.join();

        expect_eq(ok_count.load(), 1, "exactly one create");
        expect_eq(dup_count.load(), kThreads - 1, "rest duplicates");
        expect_eq(other.load(), 0, "no other errors");

        reldb::TableSchema got;
        EXPECT_OK(db->GetTable("users", &got), "get users");
        expect_eq(got.num_columns(), static_cast<std::size_t>(2), "ncols");
    }
    RemoveDirRecursive(dir);
}

// Concurrent Get through Transaction paths while another thread Creates tables.
TEST(reldb_catalog_mt_txn_lookup) {
    auto dir = MakeTempDir("reldb_cat_mt_txn");
    {
        std::shared_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");

        EXPECT_OK(db->CreateTable(TableN(0)), "seed t0");

        std::atomic<bool> stop{false};
        std::atomic<int> errors{0};

        std::thread creator([&]() {
            for (int i = 1; i < 16; ++i) {
                auto st = db->CreateTable(TableN(i));
                if (!st.ok()) errors.fetch_add(1);
            }
            stop.store(true);
        });

        std::thread reader([&]() {
            while (!stop.load()) {
                std::unique_ptr<reldb::Transaction> txn;
                auto bs = db->Begin(&txn);
                if (!bs.ok()) {
                    errors.fetch_add(1);
                    continue;
                }
                reldb::Row row;
                // Missing table is fine; any non-NotFound/OK is not.
                auto gs = txn->Get("t0", reldb::Value::Int64(1), &row);
                if (!gs.ok() && !gs.IsNotFound()) {
                    errors.fetch_add(1);
                }
                // Missing table name must be NotFound, not crash.
                auto gs2 = txn->Get("nope", reldb::Value::Int64(1), &row);
                if (!gs2.IsNotFound()) {
                    errors.fetch_add(1);
                }
                EXPECT_OK(txn->Abort(), "abort");
            }
        });

        creator.join();
        stop.store(true);
        reader.join();

        expect_eq(errors.load(), 0, "no txn catalog errors");
    }
    RemoveDirRecursive(dir);
}
