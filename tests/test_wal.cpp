#include <fstream>

#include "test_harness.h"
#include "test_util.h"
#include "lsmkv/wal.h"

TEST(wal_write_read_crc) {
    auto dir = MakeTempDir("lsmkv_wal");
    std::string path = dir + "/000001.log";
    {
        lsmkv::WalWriter w(path);
        expect(w.Open().ok(), "open w");
        expect(w.AddRecord("hello").ok(), "add1");
        expect(w.AddRecord("world").ok(), "add2");
        expect(w.Sync().ok(), "sync");
        expect(w.Close().ok(), "close");
    }
    lsmkv::WalReader r(path);
    expect(r.Open().ok(), "open r");
    std::string rec;
    expect(r.ReadRecord(&rec).ok(), "r1");
    expect_eq(rec, std::string("hello"), "hello");
    expect(r.ReadRecord(&rec).ok(), "r2");
    expect_eq(rec, std::string("world"), "world");
    expect(r.ReadRecord(&rec).IsNotFound(), "eof");
    r.Close();
    RemoveDirRecursive(dir);
}

TEST(wal_batch_encode_decode) {
    std::vector<lsmkv::BatchEntry> in = {
        {lsmkv::kTypeValue, {"a", "1"}},
        {lsmkv::kTypeDeletion, {"b", ""}},
    };
    auto bytes = lsmkv::EncodeWriteBatch(9, in);
    std::uint64_t seq = 0;
    std::vector<lsmkv::BatchEntry> out;
    expect(lsmkv::DecodeWriteBatch(bytes, &seq, &out).ok(), "decode");
    expect_eq(seq, 9ull, "seq");
    expect_eq(out.size(), std::size_t{2}, "count");
    expect_eq(out[0].second.first, std::string("a"), "key");
    expect_eq(out[0].second.second, std::string("1"), "val");
    expect(out[1].first == lsmkv::kTypeDeletion, "del");
}

TEST(wal_detects_corruption) {
    auto dir = MakeTempDir("lsmkv_walc");
    std::string path = dir + "/x.log";
    lsmkv::WalWriter w(path);
    expect(w.Open().ok(), "ow");
    expect(w.AddRecord("abcd").ok(), "add");
    w.Close();
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(8);
        f.put('Z');
    }
    lsmkv::WalReader r(path);
    expect(r.Open().ok(), "or");
    std::string rec;
    expect(r.ReadRecord(&rec).IsCorruption(), "crc fail");
    RemoveDirRecursive(dir);
}
