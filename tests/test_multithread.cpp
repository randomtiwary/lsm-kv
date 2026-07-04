
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "test_harness.h"
#include "test_util.h"
#include "lsmkv/db.h"

TEST(multithread_readers_writers) {
    auto dir = MakeTempDir("lsmkv_mt");
    lsmkv::Options opt;
    opt.create_if_missing = true;
    opt.write_buffer_size = 4 * 1024;
    opt.level0_compaction_trigger = 4;
    lsmkv::DB* db = nullptr;
    expect(lsmkv::DB::Open(opt, dir, &db).ok(), "open");

    std::atomic<bool> start{false};
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int w = 0; w < 4; ++w) {
        threads.emplace_back([&, w] {
            while (!start.load()) {}
            for (int i = 0; i < 200; ++i) {
                std::string k = "w" + std::to_string(w) + "_" + std::to_string(i);
                auto s = db->Put(lsmkv::WriteOptions(), k, "v" + std::to_string(i));
                if (!s.ok()) errors.fetch_add(1);
            }
        });
    }
    for (int r = 0; r < 4; ++r) {
        threads.emplace_back([&] {
            while (!start.load()) {}
            for (int i = 0; i < 400; ++i) {
                std::string v;
                (void)db->Get(lsmkv::ReadOptions(), "w0_0", &v);
            }
        });
    }
    start.store(true);
    for (auto& t : threads) t.join();
    expect_eq(errors.load(), 0, "no write errors");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    for (int w = 0; w < 4; ++w) {
        for (int i = 0; i < 200; i += 19) {
            std::string k = "w" + std::to_string(w) + "_" + std::to_string(i);
            std::string v;
            auto s = db->Get(lsmkv::ReadOptions(), k, &v);
            expect(s.ok(), "mt get");
            expect_eq(v, "v" + std::to_string(i), "mt val");
        }
    }
    delete db;
    RemoveDirRecursive(dir);
}
