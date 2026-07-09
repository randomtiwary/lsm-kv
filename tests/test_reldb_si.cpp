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

reldb::Database* OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    reldb::Database* db = nullptr;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

// Retry helper: begin, run body, commit; on Conflict retry.
// Under high write contention a txn may lose the first-committer-wins race many
// times in a row, so allow a generous attempt budget and yield between tries.
template <typename Fn>
bool CommitWithRetry(reldb::Database* db, Fn&& fn, int max_attempts = 10000) {
    for (int i = 0; i < max_attempts; ++i) {
        reldb::Transaction* txn = nullptr;
        if (!db->Begin(&txn).ok()) return false;
        auto st = fn(txn);
        if (!st.ok()) {
            txn->Abort();
            delete txn;
            return false;
        }
        st = txn->Commit();
        delete txn;
        if (st.ok()) return true;
        if (!st.IsConflict()) return false;
        std::this_thread::yield();
    }
    return false;
}

}  // namespace

TEST(reldb_si_concurrent_disjoint_inserts) {
    auto dir = MakeTempDir("reldb_si1");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    constexpr int kThreads = 8;
    constexpr int kPerThread = 50;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                const std::int64_t id = t * 1000 + i;
                bool ok = CommitWithRetry(db, [&](reldb::Transaction* txn) {
                    return txn->Insert("users", User(id, "u" + std::to_string(id)));
                });
                if (!ok) failures.fetch_add(1);
            }
        });
    }
    for (auto& th : threads) th.join();
    expect_eq(failures.load(), 0, "no failures");

    reldb::Transaction* txn = nullptr;
    expect(db->Begin(&txn).ok(), "begin verify");
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
    delete txn;
    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_si_concurrent_contended_updates) {
    // Many threads bump the same counter-like name field via read-modify-write.
    // Conflicts are expected; retries must converge to exactly N successful bumps.
    auto dir = MakeTempDir("reldb_si2");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* seed = nullptr;
    expect(db->Begin(&seed).ok(), "seed");
    expect(seed->Insert("users", User(1, "0")).ok(), "ins");
    expect(seed->Commit().ok(), "cseed");
    delete seed;

    constexpr int kThreads = 4;
    constexpr int kPerThread = 25;
    std::atomic<int> success{0};
    std::atomic<int> hard_fail{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kPerThread; ++i) {
                bool ok = CommitWithRetry(db, [&](reldb::Transaction* txn) -> lsmkv::Status {
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

    reldb::Transaction* txn = nullptr;
    expect(db->Begin(&txn).ok(), "verify");
    reldb::Row row;
    expect(txn->Get("users", reldb::Value::Int64(1), &row).ok(), "get");
    expect_eq(row.at(1).GetString(), std::to_string(kThreads * kPerThread), "final count");
    delete txn;
    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_si_concurrent_readers_during_writes) {
    auto dir = MakeTempDir("reldb_si3");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* seed = nullptr;
    expect(db->Begin(&seed).ok(), "seed");
    expect(seed->Insert("users", User(1, "v0")).ok(), "ins");
    expect(seed->Commit().ok(), "c");
    delete seed;

    std::atomic<bool> stop{false};
    std::atomic<int> read_ok{0};
    std::atomic<int> read_bad{0};

    std::thread writer([&]() {
        for (int i = 1; i <= 100; ++i) {
            CommitWithRetry(db, [&](reldb::Transaction* txn) {
                return txn->Update("users", User(1, "v" + std::to_string(i)));
            });
        }
        stop.store(true);
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&]() {
            while (!stop.load()) {
                reldb::Transaction* txn = nullptr;
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
                delete txn;
            }
        });
    }

    writer.join();
    for (auto& th : readers) th.join();
    expect(read_ok.load() > 0, "some reads");
    expect_eq(read_bad.load(), 0, "no bad reads");

    delete db;
    RemoveDirRecursive(dir);
}

// Educational: snapshot isolation allows write skew.
// Constraint "at least one doctor on call" can be broken by two SI transactions
// that each take one doctor off-call after seeing two on-call doctors.
TEST(reldb_si_allows_write_skew) {
    auto dir = MakeTempDir("reldb_si4");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(DoctorsSchema()).ok(), "create");

    reldb::Transaction* seed = nullptr;
    expect(db->Begin(&seed).ok(), "seed");
    expect(seed->Insert("doctors", Doctor(1, 1)).ok(), "d1");
    expect(seed->Insert("doctors", Doctor(2, 1)).ok(), "d2");
    expect(seed->Commit().ok(), "cseed");
    delete seed;

    reldb::Transaction* t1 = nullptr;
    reldb::Transaction* t2 = nullptr;
    expect(db->Begin(&t1).ok(), "t1");
    expect(db->Begin(&t2).ok(), "t2");

    // Each sees both on call.
    reldb::Row r1, r2;
    expect(t1->Get("doctors", reldb::Value::Int64(1), &r1).ok(), "t1 d1");
    expect(t1->Get("doctors", reldb::Value::Int64(2), &r2).ok(), "t1 d2");
    expect_eq(r1.at(1).GetInt64(), static_cast<std::int64_t>(1), "t1 d1 on");
    expect_eq(r2.at(1).GetInt64(), static_cast<std::int64_t>(1), "t1 d2 on");

    expect(t2->Get("doctors", reldb::Value::Int64(1), &r1).ok(), "t2 d1");
    expect(t2->Get("doctors", reldb::Value::Int64(2), &r2).ok(), "t2 d2");

    // Each takes a different doctor off call — no write-write conflict on same PK.
    expect(t1->Update("doctors", Doctor(1, 0)).ok(), "t1 off");
    expect(t2->Update("doctors", Doctor(2, 0)).ok(), "t2 off");
    expect(t1->Commit().ok(), "t1 commit");
    expect(t2->Commit().ok(), "t2 commit");  // succeeds under SI!
    delete t1;
    delete t2;

    reldb::Transaction* check = nullptr;
    expect(db->Begin(&check).ok(), "check");
    expect(check->Get("doctors", reldb::Value::Int64(1), &r1).ok(), "c1");
    expect(check->Get("doctors", reldb::Value::Int64(2), &r2).ok(), "c2");
    // Both off call — constraint violated. SI does not prevent this.
    expect_eq(r1.at(1).GetInt64() + r2.at(1).GetInt64(), static_cast<std::int64_t>(0),
              "write skew: zero on call");
    delete check;

    delete db;
    RemoveDirRecursive(dir);
}

TEST(reldb_si_lost_update_prevented) {
    // Two transactions read the same value and both try to write — one must Conflict.
    auto dir = MakeTempDir("reldb_si5");
    reldb::Database* db = OpenDb(dir);
    expect(db != nullptr, "open");
    expect(db->CreateTable(UsersSchema()).ok(), "create");

    reldb::Transaction* seed = nullptr;
    expect(db->Begin(&seed).ok(), "seed");
    expect(seed->Insert("users", User(1, "0")).ok(), "ins");
    expect(seed->Commit().ok(), "c");
    delete seed;

    reldb::Transaction* t1 = nullptr;
    reldb::Transaction* t2 = nullptr;
    expect(db->Begin(&t1).ok(), "t1");
    expect(db->Begin(&t2).ok(), "t2");
    reldb::Row row;
    expect(t1->Get("users", reldb::Value::Int64(1), &row).ok(), "r1");
    expect(t2->Get("users", reldb::Value::Int64(1), &row).ok(), "r2");
    expect(t1->Update("users", User(1, "1")).ok(), "u1");
    expect(t2->Update("users", User(1, "1")).ok(), "u2");
    expect(t1->Commit().ok(), "c1");
    expect(t2->Commit().IsConflict(), "lost update prevented");
    delete t1;
    delete t2;

    delete db;
    RemoveDirRecursive(dir);
}
