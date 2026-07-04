#include <chrono>
#include <thread>

#include "test_harness.h"
#include "test_util.h"
#include "lsmkv/db.h"

TEST(db_put_get_delete_reopen) {
    auto dir = MakeTempDir("lsmkv_db");
    lsmkv::Options opt;
    opt.create_if_missing = true;
    opt.write_buffer_size = 1024;
    lsmkv::DB* db = nullptr;
    expect(lsmkv::DB::Open(opt, dir, &db).ok(), "open");
    expect(db->Put(lsmkv::WriteOptions(), "k1", "v1").ok(), "put1");
    expect(db->Put(lsmkv::WriteOptions(), "k2", "v2").ok(), "put2");
    std::string v;
    expect(db->Get(lsmkv::ReadOptions(), "k1", &v).ok(), "get1");
    expect_eq(v, std::string("v1"), "v1");
    expect(db->Delete(lsmkv::WriteOptions(), "k1").ok(), "del");
    expect(db->Get(lsmkv::ReadOptions(), "k1", &v).IsNotFound(), "gone");
    delete db;
    expect(lsmkv::DB::Open(opt, dir, &db).ok(), "reopen");
    expect(db->Get(lsmkv::ReadOptions(), "k2", &v).ok(), "get2 after reopen");
    expect_eq(v, std::string("v2"), "v2 persist");
    expect(db->Get(lsmkv::ReadOptions(), "k1", &v).IsNotFound(), "del persist");
    delete db;
    RemoveDirRecursive(dir);
}

TEST(db_flush_to_sstable) {
    auto dir = MakeTempDir("lsmkv_dbf");
    lsmkv::Options opt;
    opt.create_if_missing = true;
    opt.write_buffer_size = 256;
    opt.level0_compaction_trigger = 100;
    lsmkv::DB* db = nullptr;
    expect(lsmkv::DB::Open(opt, dir, &db).ok(), "open");
    lsmkv::WriteOptions wo;
    wo.sync = true;
    for (int i = 0; i < 200; ++i) {
        expect(db->Put(wo, "key" + std::to_string(i), std::string(32, 'x')).ok(), "put");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::string v;
    expect(db->Get(lsmkv::ReadOptions(), "key0", &v).ok(), "get0");
    expect(db->Get(lsmkv::ReadOptions(), "key199", &v).ok(), "get199");
    delete db;
    expect(lsmkv::DB::Open(opt, dir, &db).ok(), "reopen");
    expect(db->Get(lsmkv::ReadOptions(), "key50", &v).ok(), "persist sst");
    delete db;
    RemoveDirRecursive(dir);
}

TEST(db_error_if_exists_and_missing) {
    auto dir = MakeTempDir("lsmkv_dbe");
    RemoveDirRecursive(dir);
    lsmkv::Options opt;
    opt.create_if_missing = false;
    lsmkv::DB* db = nullptr;
    expect(lsmkv::DB::Open(opt, dir, &db).IsInvalidArgument(), "missing");
    opt.create_if_missing = true;
    expect(lsmkv::DB::Open(opt, dir, &db).ok(), "create");
    delete db;
    opt.error_if_exists = true;
    expect(lsmkv::DB::Open(opt, dir, &db).IsInvalidArgument(), "exists");
    RemoveDirRecursive(dir);
}
