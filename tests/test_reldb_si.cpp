#include <memory>
#include <atomic>
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

reldb::TableSchema UsersSchema() {
    return reldb::TableSchema("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
    });
}

reldb::TableSchema DoctorsSchema() {
    // Classic write-skew example: on_call is 1 if doctor is on call.
    return reldb::TableSchema("doctors", {
        {"id", reldb::ColumnType::kInt64, true},
        {"on_call", reldb::ColumnType::kInt64, false},
    });
}

reldb::Row User(std::int64_t id, const std::string& name) {
    return reldb::Row({reldb::Value::Int64(id), reldb::Value::String(name)});
}

reldb::Row Doctor(std::int64_t id, std::int64_t on_call) {
    return reldb::Row({reldb::Value::Int64(id), reldb::Value::Int64(on_call)});
}

std::unique_ptr<reldb::Database> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::unique_ptr<reldb::Database> db;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

// Retry helper: begin, run body, commit. Retry on Conflict from either the
// write path (early WW) or commit.
template <typename Fn>
bool CommitWithRetry(reldb::Database* db, Fn&& fn, int max_attempts = 10000) {
    for (int i = 0; i < max_attempts; ++i) {
        std::unique_ptr<reldb::Transaction> txn;
        if (!db->Begin(&txn).ok()) return false;
        auto st = fn(txn.get());
        if (!st.ok()) {
            const bool retry = st.IsConflict();
            txn->Abort();
            if (!retry) return false;
            std::this_thread::yield();
            continue;
        }
        st = txn->Commit();
        if (st.ok()) return true;
        if (!st.IsConflict()) return false;
        std::this_thread::yield();
    }
    return false;
}

}  // namespace

TEST(reldb_si_concurrent_disjoint_inserts) {
    auto dir = MakeTempDir("reldb_si1");
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        constexpr int kThreads = 8;
        constexpr int kPerThread = 50;
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < kPerThread; ++i) {
                    const std::int64_t id = t * 1000 + i;
                    bool ok = CommitWithRetry(db.get(), [&](reldb::Transaction* txn) {
                        return txn->Insert("users", User(id, "u" + std::to_string(id)));
                    });
                    if (!ok) failures.fetch_add(1);
                }
            });
        }
        for (auto& th : threads) th.join();
        expect_eq(failures.load(), 0, "no failures");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin verify");
        int found = 0;
        for (int t = 0; t < kThreads; ++t) {
            for (int i = 0; i < kPerThread; ++i) {
                reldb::Row row;
                if (txn->Get("users", reldb::Value::Int64(t * 1000 + i), &row).ok()) {
                    ++found;
                }
            }
        }
        expect_eq(found, kThreads * kPerThread, "all rows");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_si_concurrent_contended_updates) {
    // Many threads bump the same counter-like name field via read-modify-write.
    // Conflicts are expected; retries must converge to exactly N successful bumps.
    auto dir = MakeTempDir("reldb_si2");
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> seed;
            EXPECT_OK(db->Begin(&seed), "seed");
            EXPECT_OK(seed->Insert("users", User(1, "0")), "ins");
            EXPECT_OK(seed->Commit(), "cseed");
        }

        constexpr int kThreads = 4;
        constexpr int kPerThread = 25;
        std::atomic<int> success{0};
        std::atomic<int> hard_fail{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&]() {
                for (int i = 0; i < kPerThread; ++i) {
                    bool ok = CommitWithRetry(db.get(), [&](reldb::Transaction* txn) -> lsmkv::Status {
                        reldb::Row row;
                        auto st = txn->Get("users", reldb::Value::Int64(1), &row);
                        if (!st.ok()) return st;
                        int cur = std::stoi(row.at(1).GetString());
                        return txn->Update("users", User(1, std::to_string(cur + 1)));
                    });
                    if (ok) {
                        success.fetch_add(1);
                    } else {
                        hard_fail.fetch_add(1);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
        expect_eq(hard_fail.load(), 0, "retries succeeded");
        expect_eq(success.load(), kThreads * kPerThread, "all bumps");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "verify");
        reldb::Row row;
        EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &row), "get");
        expect_eq(row.at(1).GetString(), std::to_string(kThreads * kPerThread), "final count");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_si_concurrent_readers_during_writes) {
    auto dir = MakeTempDir("reldb_si3");
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> seed;
            EXPECT_OK(db->Begin(&seed), "seed");
            EXPECT_OK(seed->Insert("users", User(1, "v0")), "ins");
            EXPECT_OK(seed->Commit(), "c");
        }

        std::atomic<bool> stop{false};
        std::atomic<int> read_ok{0};
        std::atomic<int> read_bad{0};

        std::thread writer([&]() {
            for (int i = 1; i <= 100; ++i) {
                CommitWithRetry(db.get(), [&](reldb::Transaction* txn) {
                    return txn->Update("users", User(1, "v" + std::to_string(i)));
                });
            }
            stop.store(true);
        });

        std::vector<std::thread> readers;
        for (int r = 0; r < 4; ++r) {
            readers.emplace_back([&]() {
                while (!stop.load()) {
                    std::unique_ptr<reldb::Transaction> txn;
                    if (!db->Begin(&txn).ok()) {
                        read_bad.fetch_add(1);
                        continue;
                    }
                    reldb::Row row;
                    auto st = txn->Get("users", reldb::Value::Int64(1), &row);
                    // Under SI, either NotFound (shouldn't) or a consistent value "vN".
                    if (!st.ok()) {
                        read_bad.fetch_add(1);
                    } else {
                        const auto& s = row.at(1).GetString();
                        if (s.size() >= 2 && s[0] == 'v') {
                            read_ok.fetch_add(1);
                        } else {
                            read_bad.fetch_add(1);
                        }
                    }
                    txn->Abort();
                }
            });
        }

        writer.join();
        for (auto& th : readers) th.join();
        expect(read_ok.load() > 0, "some reads");
        expect_eq(read_bad.load(), 0, "no bad reads");
    }
    RemoveDirRecursive(dir);
}

// Educational: snapshot isolation allows write skew.
// Constraint "at least one doctor on call" can be broken by two SI transactions
// that each take one doctor off-call after seeing two on-call doctors.
TEST(reldb_si_allows_write_skew) {
    auto dir = MakeTempDir("reldb_si4");
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(DoctorsSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> seed;
            EXPECT_OK(db->Begin(&seed), "seed");
            EXPECT_OK(seed->Insert("doctors", Doctor(1, 1)), "d1");
            EXPECT_OK(seed->Insert("doctors", Doctor(2, 1)), "d2");
            EXPECT_OK(seed->Commit(), "cseed");
        }

        std::unique_ptr<reldb::Transaction> t1;
        std::unique_ptr<reldb::Transaction> t2;
        EXPECT_OK(db->Begin(&t1), "t1");
        EXPECT_OK(db->Begin(&t2), "t2");

        // Each sees both on call.
        reldb::Row r1, r2;
        EXPECT_OK(t1->Get("doctors", reldb::Value::Int64(1), &r1), "t1 d1");
        EXPECT_OK(t1->Get("doctors", reldb::Value::Int64(2), &r2), "t1 d2");
        expect_eq(r1.at(1).GetInt64(), static_cast<std::int64_t>(1), "t1 d1 on");
        expect_eq(r2.at(1).GetInt64(), static_cast<std::int64_t>(1), "t1 d2 on");

        EXPECT_OK(t2->Get("doctors", reldb::Value::Int64(1), &r1), "t2 d1");
        EXPECT_OK(t2->Get("doctors", reldb::Value::Int64(2), &r2), "t2 d2");

        // Each takes a different doctor off call — no write-write conflict on same PK.
        EXPECT_OK(t1->Update("doctors", Doctor(1, 0)), "t1 off");
        EXPECT_OK(t2->Update("doctors", Doctor(2, 0)), "t2 off");
        EXPECT_OK(t1->Commit(), "t1 commit");
        EXPECT_OK(t2->Commit(), "t2 commit");  // succeeds under SI!

        std::unique_ptr<reldb::Transaction> check;
        EXPECT_OK(db->Begin(&check), "check");
        EXPECT_OK(check->Get("doctors", reldb::Value::Int64(1), &r1), "c1");
        EXPECT_OK(check->Get("doctors", reldb::Value::Int64(2), &r2), "c2");
        // Both off call — constraint violated. SI does not prevent this.
        expect_eq(r1.at(1).GetInt64() + r2.at(1).GetInt64(), static_cast<std::int64_t>(0),
                  "write skew: zero on call");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_si_lost_update_prevented) {
    // Two transactions read the same value and both try to write — one must Conflict.
    auto dir = MakeTempDir("reldb_si5");
    {
        std::unique_ptr<reldb::Database> db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> seed;
            EXPECT_OK(db->Begin(&seed), "seed");
            EXPECT_OK(seed->Insert("users", User(1, "0")), "ins");
            EXPECT_OK(seed->Commit(), "c");
        }

        std::unique_ptr<reldb::Transaction> t1;
        std::unique_ptr<reldb::Transaction> t2;
        EXPECT_OK(db->Begin(&t1), "t1");
        EXPECT_OK(db->Begin(&t2), "t2");
        reldb::Row row;
        EXPECT_OK(t1->Get("users", reldb::Value::Int64(1), &row), "r1");
        EXPECT_OK(t2->Get("users", reldb::Value::Int64(1), &row), "r2");
        EXPECT_OK(t1->Update("users", User(1, "1")), "u1");
        // Early WW: second updater conflicts on write while t1 still holds the key.
        expect(t2->Update("users", User(1, "1")).IsConflict(), "u2 early ww");
        EXPECT_OK(t1->Commit(), "c1");
        EXPECT_OK(t2->Abort(), "t2 abort");
    }
    RemoveDirRecursive(dir);
}
