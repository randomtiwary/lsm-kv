
#include "test_harness.h"
#include "test_util.h"
#include "lsmkv/internal_key.h"
#include "lsmkv/memtable.h"
#include "lsmkv/sstable.h"

TEST(sstable_build_and_get) {
    auto dir = MakeTempDir("lsmkv_sst");
    lsmkv::Options opt;
    opt.block_size = 64;
    opt.block_restart_interval = 2;
    std::string path = dir + "/1.sst";
    lsmkv::SSTableBuilder b(opt, path);
    expect(b.Open().ok(), "open");
    auto key_of = [](int i) {
        std::string k = std::to_string(i);
        return std::string(3 - k.size(), '0') + k;
    };
    for (int i = 0; i < 50; ++i) {
        auto ik = lsmkv::MakeInternalKey(key_of(i), i + 1, lsmkv::kTypeValue);
        b.Add(ik, "v" + std::to_string(i));
    }
    lsmkv::FileMetaData meta;
    expect(b.Finish(&meta).ok(), "finish");
    expect(meta.file_size > 0, "size");
    std::unique_ptr<lsmkv::SSTable> table;
    expect(lsmkv::SSTable::Open(path, &table).ok(), "open table");
    std::string val;
    bool found = false;
    auto ik = lsmkv::MakeLookupKey(key_of(10), 100);
    expect(table->Get(ik, &val, &found).ok(), "get");
    expect(found, "found");
    expect_eq(val, std::string("v10"), "val");
    auto it = table->NewIterator();
    it->SeekToFirst();
    int n = 0;
    while (it->Valid()) { ++n; it->Next(); }
    expect_eq(n, 50, "iter all");
    RemoveDirRecursive(dir);
}

TEST(sstable_from_memtable) {
    auto dir = MakeTempDir("lsmkv_sstm");
    lsmkv::MemTable mt;
    mt.Add(1, lsmkv::kTypeValue, "x", "1");
    mt.Add(2, lsmkv::kTypeValue, "y", "2");
    lsmkv::FileMetaData meta;
    lsmkv::Options opt;
    expect(lsmkv::BuildTableFromMemTable(opt, dir + "/2.sst", mt, 2, &meta).ok(), "build");
    expect_eq(meta.number, 2ull, "num");
    expect(!meta.smallest.empty(), "smallest");
    RemoveDirRecursive(dir);
}

TEST(sstable_large_write_and_iterate) {
    constexpr int kRows = 10000;
    auto dir = MakeTempDir("lsmkv_sst_large");
    lsmkv::Options opt;
    opt.block_size = 256;  // force many data blocks / index entries
    opt.block_restart_interval = 16;
    std::string path = dir + "/large.sst";

    auto user_key = [](int i) {
        // Fixed-width decimal keys so lexicographic order matches numeric order.
        std::string k = std::to_string(i);
        return std::string(5 - k.size(), '0') + k;
    };
    auto value_of = [](int i) { return "val-" + std::to_string(i); };

    {
        lsmkv::SSTableBuilder b(opt, path);
        expect(b.Open().ok(), "open builder");
        for (int i = 0; i < kRows; ++i) {
            auto ik = lsmkv::MakeInternalKey(user_key(i), static_cast<std::uint64_t>(i + 1),
                                             lsmkv::kTypeValue);
            b.Add(ik, value_of(i));
        }
        lsmkv::FileMetaData meta;
        expect(b.Finish(&meta).ok(), "finish");
        expect(meta.file_size > 0, "non-empty file");
        expect_eq(lsmkv::ExtractUserKey(meta.smallest).ToString(), user_key(0), "smallest");
        expect_eq(lsmkv::ExtractUserKey(meta.largest).ToString(), user_key(kRows - 1), "largest");
    }

    std::unique_ptr<lsmkv::SSTable> table;
    expect(lsmkv::SSTable::Open(path, &table).ok(), "open table");

    auto it = table->NewIterator();
    it->SeekToFirst();
    expect(it->status().ok(), "iter status start");
    int n = 0;
    std::string prev;
    while (it->Valid()) {
        expect(it->status().ok(), "iter status");
        auto uk = lsmkv::ExtractUserKey(it->key()).ToString();
        expect_eq(uk, user_key(n), "iter key order");
        expect_eq(it->value().ToString(), value_of(n), "iter value");
        if (n > 0) expect(uk > prev, "strictly increasing user keys");
        prev = uk;
        ++n;
        it->Next();
    }
    expect(it->status().ok(), "iter status end");
    expect_eq(n, kRows, "iterated all rows");

    // Spot-check point lookups across the key space, including block boundaries.
    for (int i : {0, 1, 256, 1000, 5000, 9998, 9999}) {
        std::string val;
        bool found = false;
        auto lk = lsmkv::MakeLookupKey(user_key(i), static_cast<std::uint64_t>(kRows + 1));
        expect(table->Get(lk, &val, &found).ok(), "get ok");
        expect(found, "get found");
        expect_eq(val, value_of(i), "get value");
    }

    // Seek into the middle and finish the scan.
    it->Seek(lsmkv::MakeLookupKey(user_key(7500), static_cast<std::uint64_t>(kRows + 1)));
    expect(it->Valid(), "mid seek valid");
    expect_eq(lsmkv::ExtractUserKey(it->key()).ToString(), user_key(7500), "mid seek key");
    int from_mid = 0;
    while (it->Valid()) {
        ++from_mid;
        it->Next();
    }
    expect_eq(from_mid, kRows - 7500, "rows from mid seek");

    RemoveDirRecursive(dir);
}
