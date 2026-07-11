#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "test_harness.h"
#include "test_util.h"

#include "lsmkv/db.h"

namespace {

std::unique_ptr<lsmkv::DB> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    lsmkv::DB* raw = nullptr;
    if (!lsmkv::DB::Open(opt, dir, &raw).ok()) return nullptr;
    return std::unique_ptr<lsmkv::DB>(raw);
}

}  // namespace

TEST(db_iterator_empty) {
    auto dir = MakeTempDir("lsmkv_it_empty");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        auto it = db->NewIterator(lsmkv::ReadOptions());
        expect(it != nullptr, "iter");
        it->SeekToFirst();
        expect(!it->Valid(), "empty");
        EXPECT_OK(it->status(), "status");
    }
    RemoveDirRecursive(dir);
}

TEST(db_iterator_seek_and_order) {
    auto dir = MakeTempDir("lsmkv_it_order");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->Put(lsmkv::WriteOptions(), "b", "2"), "put b");
        EXPECT_OK(db->Put(lsmkv::WriteOptions(), "a", "1"), "put a");
        EXPECT_OK(db->Put(lsmkv::WriteOptions(), "c", "3"), "put c");

        auto it = db->NewIterator(lsmkv::ReadOptions());
        it->SeekToFirst();
        expect(it->Valid(), "first");
        expect_eq(it->key().ToString(), std::string("a"), "key a");
        expect_eq(it->value().ToString(), std::string("1"), "val a");
        it->Next();
        expect(it->Valid(), "second");
        expect_eq(it->key().ToString(), std::string("b"), "key b");
        it->Next();
        expect(it->Valid(), "third");
        expect_eq(it->key().ToString(), std::string("c"), "key c");
        it->Next();
        expect(!it->Valid(), "end");

        it->Seek("b");
        expect(it->Valid(), "seek b");
        expect_eq(it->key().ToString(), std::string("b"), "seek key");
        expect_eq(it->value().ToString(), std::string("2"), "seek val");

        it->Seek("b\xff");  // after b
        expect(it->Valid(), "seek after b");
        expect_eq(it->key().ToString(), std::string("c"), "land on c");

        it->Seek("z");
        expect(!it->Valid(), "seek past end");
    }
    RemoveDirRecursive(dir);
}

TEST(db_iterator_matches_get_and_hides_tombstones) {
    auto dir = MakeTempDir("lsmkv_it_get");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->Put(lsmkv::WriteOptions(), "keep", "yes"), "put keep");
        EXPECT_OK(db->Put(lsmkv::WriteOptions(), "gone", "no"), "put gone");
        EXPECT_OK(db->Delete(lsmkv::WriteOptions(), "gone"), "del gone");
        EXPECT_OK(db->Put(lsmkv::WriteOptions(), "keep", "yes2"), "overwrite keep");

        std::string v;
        expect(db->Get(lsmkv::ReadOptions(), "gone", &v).IsNotFound(), "get gone");
        EXPECT_OK(db->Get(lsmkv::ReadOptions(), "keep", &v), "get keep");
        expect_eq(v, std::string("yes2"), "get value");

        std::map<std::string, std::string> seen;
        auto it = db->NewIterator(lsmkv::ReadOptions());
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            seen[it->key().ToString()] = it->value().ToString();
        }
        EXPECT_OK(it->status(), "iter status");
        expect_eq(static_cast<int>(seen.size()), 1, "one live key");
        expect_eq(seen["keep"], std::string("yes2"), "iter value");
        expect(seen.count("gone") == 0, "no tombstone");
    }
    RemoveDirRecursive(dir);
}

TEST(db_iterator_across_flush) {
    auto dir = MakeTempDir("lsmkv_it_flush");
    {
        lsmkv::Options opt;
        opt.create_if_missing = true;
        opt.write_buffer_size = 256;
        opt.level0_compaction_trigger = 100;
        lsmkv::DB* raw = nullptr;
        EXPECT_OK(lsmkv::DB::Open(opt, dir, &raw), "open");
        std::unique_ptr<lsmkv::DB> db(raw);

        lsmkv::WriteOptions wo;
        wo.sync = true;
        for (int i = 0; i < 80; ++i) {
            EXPECT_OK(db->Put(wo, "k" + std::to_string(i), std::string(40, 'x')), "put");
        }
        // Allow background flush some time (same pattern as other DB tests).
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        int count = 0;
        auto it = db->NewIterator(lsmkv::ReadOptions());
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            ++count;
            std::string via_get;
            EXPECT_OK(db->Get(lsmkv::ReadOptions(), it->key(), &via_get), "get match");
            expect_eq(via_get, it->value().ToString(), "value match");
        }
        EXPECT_OK(it->status(), "status");
        expect(count >= 80, "at least 80 keys");
    }
    RemoveDirRecursive(dir);
}
