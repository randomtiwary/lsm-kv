#include <chrono>
#include <thread>

#include "test_harness.h"
#include "test_util.h"
#include "lsmkv/db.h"

TEST(compaction_moves_to_l1) {
    auto dir = MakeTempDir("lsmkv_cmp");
    lsmkv::Options opt;
    opt.create_if_missing = true;
    opt.write_buffer_size = 200;
    opt.level0_compaction_trigger = 2;
    lsmkv::DB* db = nullptr;
    expect(lsmkv::DB::Open(opt, dir, &db).ok(), "open");
    lsmkv::WriteOptions wo; wo.sync = true;
    for (int i = 0; i < 500; ++i) {
        expect(db->Put(wo, "k" + std::to_string(i), std::string(64, 'a' + (i % 26))).ok(), "put");
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::string v;
    for (int i = 0; i < 500; i += 17) {
        expect(db->Get(lsmkv::ReadOptions(), "k" + std::to_string(i), &v).ok(), "get");
    }
    // overwrite and delete some
    expect(db->Put(wo, "k0", "new").ok(), "overwrite");
    expect(db->Delete(wo, "k1").ok(), "delete");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    expect(db->Get(lsmkv::ReadOptions(), "k0", &v).ok(), "get new");
    expect_eq(v, std::string("new"), "new val");
    expect(db->Get(lsmkv::ReadOptions(), "k1", &v).IsNotFound(), "deleted");
    delete db;
    expect(lsmkv::DB::Open(opt, dir, &db).ok(), "reopen");
    expect(db->Get(lsmkv::ReadOptions(), "k0", &v).ok(), "persist new");
    expect_eq(v, std::string("new"), "persist val");
    expect(db->Get(lsmkv::ReadOptions(), "k1", &v).IsNotFound(), "persist del");
    delete db;
    RemoveDirRecursive(dir);
}
