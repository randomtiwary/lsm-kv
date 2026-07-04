#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "test_harness.h"
#include "lsmkv/memtable.h"

TEST(memtable_put_get_delete) {
    lsmkv::MemTable mt;
    mt.Add(1, lsmkv::kTypeValue, "k", "v1");
    mt.Add(2, lsmkv::kTypeValue, "k", "v2");
    mt.Add(3, lsmkv::kTypeDeletion, "k", "");
    std::optional<std::string> v;
    bool found = false;
    expect(mt.Get("k", 1, &v, &found).ok() && found && v.has_value(), "seq1 ok");
    expect_eq(*v, std::string("v1"), "v1");
    expect(mt.Get("k", 2, &v, &found).ok() && found && v.has_value(), "seq2 ok");
    expect_eq(*v, std::string("v2"), "v2");
    expect(mt.Get("k", 3, &v, &found).ok() && found && !v.has_value(), "deleted");
    expect(mt.Get("missing", 10, &v, &found).ok() && !found && !v.has_value(), "missing");
}

TEST(memtable_deletion_tombstone) {
    lsmkv::MemTable mt;
    mt.Add(1, lsmkv::kTypeValue, "x", "alive");
    expect_eq(mt.EntryCount(), std::size_t{1}, "one entry before delete");

    mt.Add(2, lsmkv::kTypeDeletion, "x", "");
    expect_eq(mt.EntryCount(), std::size_t{2}, "tombstone is an extra entry");

    std::optional<std::string> v;
    bool found = false;
    expect(mt.Get("x", 2, &v, &found).ok() && found && !v.has_value(), "latest sees tombstone");
    expect(mt.Get("x", 1, &v, &found).ok() && found && v.has_value(), "older snapshot still sees value");
    expect_eq(*v, std::string("alive"), "pre-delete value");

    mt.Add(3, lsmkv::kTypeValue, "x", "reborn");
    expect(mt.Get("x", 3, &v, &found).ok() && found && v.has_value(), "resurrected");
    expect_eq(*v, std::string("reborn"), "new value");
    expect(mt.Get("x", 2, &v, &found).ok() && found && !v.has_value(), "mid snapshot still deleted");
}

TEST(memtable_empty_string_value_vs_tombstone) {
    lsmkv::MemTable mt;
    mt.Add(1, lsmkv::kTypeValue, "e", "");
    std::optional<std::string> v;
    bool found = false;
    expect(mt.Get("e", 1, &v, &found).ok() && found && v.has_value(), "empty live value");
    expect(v->empty(), "empty string payload");

    mt.Add(2, lsmkv::kTypeDeletion, "e", "");
    expect(mt.Get("e", 2, &v, &found).ok() && found && !v.has_value(), "tombstone is nullopt");
}

TEST(memtable_many_keys_ordered) {
    lsmkv::MemTable mt;
    for (int i = 100; i >= 0; --i) mt.Add(i + 1, lsmkv::kTypeValue, std::to_string(i), "x");
    std::string prev;
    bool first = true;
    int n = 0;
    mt.ForEach([&](lsmkv::Slice ikey, lsmkv::Slice) {
        auto uk = lsmkv::ExtractUserKey(ikey).ToString();
        if (!first) expect(uk >= prev, "ordered user keys");
        prev = uk;
        first = false;
        ++n;
    });
    expect(n == 101, "count");
    expect(mt.ApproximateMemoryUsage() > 0, "memory tracked");
}

TEST(memtable_concurrent_put_get) {
    constexpr int kWriters = 4;
    constexpr int kPerWriter = 250;
    lsmkv::MemTable mt;
    std::atomic<bool> start{false};
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            while (!start.load()) {
            }
            for (int i = 0; i < kPerWriter; ++i) {
                std::uint64_t seq = static_cast<std::uint64_t>(w * kPerWriter + i + 1);
                std::string key = "k" + std::to_string(w) + "_" + std::to_string(i);
                std::string val = "v" + std::to_string(i);
                mt.Add(seq, lsmkv::kTypeValue, key, val);
            }
        });
    }

    for (int r = 0; r < 4; ++r) {
        threads.emplace_back([&] {
            while (!start.load()) {
            }
            for (int i = 0; i < 500; ++i) {
                std::optional<std::string> v;
                bool found = false;
                auto s = mt.Get("missing", static_cast<std::uint64_t>(kWriters * kPerWriter + 1), &v,
                                &found);
                if (!s.ok() || found || v.has_value()) errors.fetch_add(1);
            }
        });
    }

    start.store(true);
    for (auto& th : threads) th.join();

    expect_eq(errors.load(), 0, "no concurrent reader errors");
    expect_eq(mt.EntryCount(), static_cast<std::size_t>(kWriters * kPerWriter), "all inserts");

    std::optional<std::string> v;
    bool found = false;
    std::uint64_t snapshot = static_cast<std::uint64_t>(kWriters * kPerWriter);
    for (int w = 0; w < kWriters; ++w) {
        for (int i = 0; i < kPerWriter; i += 17) {
            std::string key = "k" + std::to_string(w) + "_" + std::to_string(i);
            expect(mt.Get(key, snapshot, &v, &found).ok() && found && v.has_value(), "concurrent get");
            expect_eq(*v, "v" + std::to_string(i), "concurrent value");
        }
    }
}

TEST(memtable_concurrent_put_get_delete) {
    lsmkv::MemTable mt;
    constexpr int kKeys = 200;
    for (int i = 0; i < kKeys; ++i) {
        mt.Add(static_cast<std::uint64_t>(i + 1), lsmkv::kTypeValue, "k" + std::to_string(i),
               "v" + std::to_string(i));
    }

    std::atomic<bool> start{false};
    std::atomic<std::uint64_t> next_seq{kKeys + 1};
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    threads.emplace_back([&] {
        while (!start.load()) {
        }
        for (int i = 0; i < kKeys; i += 2) {
            auto seq = next_seq.fetch_add(1);
            mt.Add(seq, lsmkv::kTypeDeletion, "k" + std::to_string(i), "");
        }
    });

    threads.emplace_back([&] {
        while (!start.load()) {
        }
        for (int i = 1; i < kKeys; i += 2) {
            auto seq = next_seq.fetch_add(1);
            mt.Add(seq, lsmkv::kTypeValue, "k" + std::to_string(i), "new" + std::to_string(i));
        }
    });

    for (int r = 0; r < 4; ++r) {
        threads.emplace_back([&] {
            while (!start.load()) {
            }
            for (int round = 0; round < 100; ++round) {
                std::optional<std::string> v;
                bool found = false;
                auto snap = next_seq.load();
                if (snap == 0) continue;
                --snap;
                auto s = mt.Get("k0", snap, &v, &found);
                if (!s.ok()) errors.fetch_add(1);
            }
        });
    }

    start.store(true);
    for (auto& th : threads) th.join();
    expect_eq(errors.load(), 0, "no races producing corruption");

    std::optional<std::string> v;
    bool found = false;
    auto snap = next_seq.load() - 1;
    expect(mt.Get("k0", snap, &v, &found).ok() && found && !v.has_value(), "even key deleted");
    expect(mt.Get("k1", snap, &v, &found).ok() && found && v.has_value(), "odd key updated");
    expect_eq(*v, std::string("new1"), "updated value");
}
