#include <atomic>
#include <thread>
#include <vector>

#include "test_harness.h"
#include "lsmkv/skiplist.h"

struct IntCmp {
    int operator()(int a, int b) const { return (a < b) ? -1 : (a > b ? 1 : 0); }
};

TEST(skiplist_insert_contains_iterate) {
    constexpr int kCount = 10000;
    lsmkv::SkipList<int, IntCmp> list;
    for (int i = 0; i < kCount; ++i) list.Insert(i * 2);

    expect(list.Contains(0), "has first even");
    expect(list.Contains(9998), "has near-last even");
    expect(list.Contains((kCount - 1) * 2), "has last even");
    expect(!list.Contains(1), "no odd key");
    expect(!list.Contains(kCount * 2), "no key past end");
    expect_eq(list.Size(), static_cast<std::size_t>(kCount), "size");

    int prev = -1;
    int count = 0;
    list.Iterate([&](int v) {
        expect(v > prev, "ordered");
        expect_eq(v, count * 2, "dense even sequence");
        prev = v;
        ++count;
    });
    expect_eq(count, kCount, "iter count");
}

TEST(skiplist_find) {
    lsmkv::SkipList<int, IntCmp> list;
    for (int i = 0; i < 100; i += 2) list.Insert(i);

    int out = -1;
    expect(list.Find(0, &out), "find exact min");
    expect_eq(out, 0, "min value");

    expect(list.Find(50, &out), "find exact middle");
    expect_eq(out, 50, "middle value");

    expect(list.Find(98, &out), "find exact max");
    expect_eq(out, 98, "max value");

    // Find returns the first entry >= target.
    expect(list.Find(49, &out), "find ceiling of missing odd");
    expect_eq(out, 50, "ceiling is next even");

    expect(list.Find(1, &out), "find ceiling of 1");
    expect_eq(out, 2, "ceiling of 1 is 2");

    expect(!list.Find(100, &out), "find past end is false");
    expect(!list.Find(1000, &out), "find far past end is false");
}

TEST(skiplist_find_empty_and_single) {
    lsmkv::SkipList<int, IntCmp> empty;
    int out = 42;
    expect(!empty.Find(0, &out), "empty find fails");
    expect_eq(out, 42, "out unchanged on failure");

    lsmkv::SkipList<int, IntCmp> single;
    single.Insert(7);
    expect(single.Find(7, &out), "find only element");
    expect_eq(out, 7, "only element value");
    expect(single.Find(0, &out), "find below only element");
    expect_eq(out, 7, "ceiling is only element");
    expect(!single.Find(8, &out), "find above only element fails");
}

TEST(skiplist_update_equal_key) {
    struct Pair {
        int k;
        int v;
    };
    struct Cmp {
        int operator()(const Pair& a, const Pair& b) const {
            return (a.k < b.k) ? -1 : (a.k > b.k ? 1 : 0);
        }
    };
    lsmkv::SkipList<Pair, Cmp> list;
    list.Insert(Pair{1, 10});
    list.Insert(Pair{1, 20});
    expect_eq(list.Size(), std::size_t{1}, "one entry");
    Pair out{0, 0};
    expect(list.Find(Pair{1, 0}, &out), "find");
    expect_eq(out.v, 20, "updated");

    // Find with a lower payload still matches by key comparator.
    expect(list.Find(Pair{1, -1}, &out), "find ignores value field");
    expect_eq(out.v, 20, "still updated value");
    expect(!list.Find(Pair{2, 0}, &out), "missing key");
}

TEST(skiplist_concurrent_readers_writers) {
    lsmkv::SkipList<int, IntCmp> list;
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            while (!start.load()) {}
            for (int i = 0; i < 200; ++i) list.Insert(t * 1000 + i);
        });
    }
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            while (!start.load()) {}
            for (int i = 0; i < 500; ++i) {
                int out = -1;
                (void)list.Contains(i);
                (void)list.Find(i, &out);
            }
        });
    }
    start.store(true);
    for (auto& th : threads) th.join();
    expect_eq(list.Size(), std::size_t{800}, "all inserts");

    int out = -1;
    expect(list.Find(0, &out), "find after concurrent inserts");
    expect_eq(out, 0, "found 0");
    expect(list.Find(3199, &out), "find last writer key");
    expect_eq(out, 3199, "found 3199");
}

TEST(skiplist_iterator_basic) {
    lsmkv::SkipList<int, IntCmp> list;
    for (int i = 0; i < 10; ++i) list.Insert(i);

    auto it = list.NewIterator();
    it.SeekToFirst();
    int count = 0;
    int prev = -1;
    while (it.Valid()) {
        expect(it.key() > prev, "iterator ordered");
        prev = it.key();
        ++count;
        it.Next();
    }
    expect_eq(count, 10, "iterator count");

    it.Seek(5);
    expect(it.Valid(), "seek hit");
    expect_eq(it.key(), 5, "seek value");
    it.Seek(100);
    expect(!it.Valid(), "seek past end");
}

TEST(skiplist_concurrent_iterators) {
    constexpr int kCount = 5000;
    constexpr int kIterators = 4;
    constexpr int kReaders = 2;
    lsmkv::SkipList<int, IntCmp> list;
    for (int i = 0; i < kCount; ++i) list.Insert(i);

    // Phased synchronization:
    //   1. Iterators (and point-readers) acquire shared locks and announce ready.
    //   2. Only then does the writer call Insert, which must block on the exclusive
    //      lock until every shared lock is released.
    // Without phase 1, the writer can win the race, insert kCount+1 first, and
    // full-table scans observe kCount+1 entries — a flaky false failure.
    std::atomic<int> iterators_ready{0};
    std::atomic<int> readers_ready{0};
    std::atomic<bool> release_iterators{false};
    std::atomic<int> errors{0};
    std::atomic<bool> writer_entered_insert{false};
    std::atomic<bool> writer_finished{false};
    std::atomic<bool> saw_writer_blocked{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < kIterators; ++t) {
        threads.emplace_back([&, t] {
            auto it = list.NewIterator();  // acquires shared_lock
            iterators_ready.fetch_add(1, std::memory_order_release);
            while (!release_iterators.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (writer_finished.load(std::memory_order_acquire)) {
                // Insert must not complete while any iterator still holds its lock.
                errors.fetch_add(1);
            }
            if (t % 2 == 0) {
                it.SeekToFirst();
            } else {
                it.Seek(kCount / 2);
            }
            int prev = -1;
            int seen = 0;
            while (it.Valid()) {
                if (it.key() <= prev) errors.fetch_add(1);
                prev = it.key();
                ++seen;
                it.Next();
            }
            const int expected = (t % 2 == 0) ? kCount : (kCount - kCount / 2);
            if (seen != expected) errors.fetch_add(1);
            // Destroying `it` drops the shared_lock.
        });
    }

    // Point readers coexist with live iterators under shared locks.
    for (int t = 0; t < kReaders; ++t) {
        threads.emplace_back([&] {
            readers_ready.fetch_add(1, std::memory_order_release);
            while (!release_iterators.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 200; ++i) {
                int out = -1;
                if (!list.Contains(i % kCount)) errors.fetch_add(1);
                if (!list.Find(i % kCount, &out) || out != (i % kCount)) errors.fetch_add(1);
            }
        });
    }

    threads.emplace_back([&] {
        while (iterators_ready.load(std::memory_order_acquire) < kIterators ||
               readers_ready.load(std::memory_order_acquire) < kReaders) {
            std::this_thread::yield();
        }
        writer_entered_insert.store(true, std::memory_order_release);
        list.Insert(kCount + 1);  // blocks until all shared locks are dropped
        writer_finished.store(true, std::memory_order_release);
    });

    while (iterators_ready.load(std::memory_order_acquire) < kIterators ||
           readers_ready.load(std::memory_order_acquire) < kReaders) {
        std::this_thread::yield();
    }
    // Give the writer time to block inside Insert while shared locks are held.
    for (int spin = 0; spin < 1000; ++spin) {
        if (writer_entered_insert.load(std::memory_order_acquire) &&
            !writer_finished.load(std::memory_order_acquire)) {
            saw_writer_blocked.store(true, std::memory_order_release);
            break;
        }
        std::this_thread::yield();
    }
    expect(!writer_finished.load(std::memory_order_acquire),
           "writer must still be blocked while iterators hold shared locks");

    release_iterators.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    expect_eq(errors.load(), 0, "no iterator/reader errors");
    expect(writer_entered_insert.load(), "writer attempted insert");
    expect(writer_finished.load(), "writer completed after iterators released locks");
    expect(saw_writer_blocked.load(), "observed writer blocked behind shared locks");
    expect(list.Contains(kCount + 1), "writer insert visible");
    expect_eq(list.Size(), static_cast<std::size_t>(kCount + 1), "size includes writer insert");
}
