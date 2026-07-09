#include "test_harness.h"
#include "test_util.h"

#include "lsmkv/db.h"
#include "reldb/mvcc.h"
#include "reldb/row.h"
#include "reldb/types.h"

// --- Pure visibility (no KV) -------------------------------------------------

TEST(reldb_mvcc_visibility_basic) {
    reldb::VersionRecord v;
    v.start_ts = 10;
    v.end_ts = 0;  // live forever after 10

    expect(reldb::IsVisible(v, 10), "at start");
    expect(reldb::IsVisible(v, 100), "after start");
    expect(!reldb::IsVisible(v, 9), "before start");

    v.end_ts = 20;
    expect(reldb::IsVisible(v, 10), "at start closed");
    expect(reldb::IsVisible(v, 19), "before end");
    expect(!reldb::IsVisible(v, 20), "at end exclusive");
    expect(!reldb::IsVisible(v, 21), "after end");
    expect(!reldb::IsVisible(v, 9), "before start closed");
}

TEST(reldb_mvcc_version_record_codec) {
    reldb::VersionRecord v;
    v.start_ts = 5;
    v.end_ts = 9;
    v.prev_ts = 3;
    v.is_tombstone = false;
    v.payload = reldb::Row({reldb::Value::Int64(1), reldb::Value::String("x")}).Encode();

    reldb::VersionRecord out;
    expect(reldb::VersionRecord::Decode(v.Encode(), &out).ok(), "decode");
    expect_eq(out.start_ts, static_cast<std::uint64_t>(5), "start");
    expect_eq(out.end_ts, static_cast<std::uint64_t>(9), "end");
    expect_eq(out.prev_ts, static_cast<std::uint64_t>(3), "prev");
    expect(!out.is_tombstone, "not tomb");
    expect_eq(out.payload, v.payload, "payload");

    v.is_tombstone = true;
    v.payload.clear();
    expect(reldb::VersionRecord::Decode(v.Encode(), &out).ok(), "tomb decode");
    expect(out.is_tombstone, "tomb");
}

TEST(reldb_mvcc_pk_key_encoding) {
    reldb::Value pk = reldb::Value::String("a/b");
    std::string hex = reldb::EncodePkForKey(pk);
    expect(hex.find('/') == std::string::npos, "no slash in hex");
    reldb::Value back;
    expect(reldb::DecodePkFromKey(hex, &back).ok(), "decode pk");
    expect(back == pk, "pk roundtrip");

    reldb::Value i = reldb::Value::Int64(42);
    expect(reldb::DecodePkFromKey(reldb::EncodePkForKey(i), &back).ok(), "int pk");
    expect(back == i, "int eq");
}

// --- Store integration -------------------------------------------------------

namespace {

lsmkv::DB* OpenTemp(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    lsmkv::DB* db = nullptr;
    if (!lsmkv::DB::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

reldb::Row MakeUser(std::int64_t id, const std::string& name) {
    return reldb::Row({reldb::Value::Int64(id), reldb::Value::String(name)});
}

}  // namespace

TEST(reldb_mvcc_insert_and_read_at_snapshot) {
    auto dir = MakeTempDir("reldb_mvcc1");
    lsmkv::DB* kv = OpenTemp(dir);
    expect(kv != nullptr, "open");
    reldb::MvccStore store(kv);

    reldb::Value pk = reldb::Value::Int64(1);
    reldb::VersionRecord v1;
    v1.start_ts = 10;
    v1.end_ts = 0;
    v1.prev_ts = 0;
    v1.payload = MakeUser(1, "ann").Encode();
    expect(store.PutVersion("users", pk, v1).ok(), "put v1");

    reldb::Row row;
    expect(store.GetRow("users", pk, /*snapshot=*/10, &row).ok(), "snap 10");
    expect(row.at(1) == reldb::Value::String("ann"), "ann");
    expect(store.GetRow("users", pk, /*snapshot=*/9, &row).IsNotFound(), "snap 9");
    expect(store.GetRow("users", pk, /*snapshot=*/100, &row).ok(), "snap 100");

    std::uint64_t latest = 0;
    expect(store.GetLatestStartTs("users", pk, &latest).ok(), "latest");
    expect_eq(latest, static_cast<std::uint64_t>(10), "latest 10");

    delete kv;
    RemoveDirRecursive(dir);
}

TEST(reldb_mvcc_update_chain_visibility) {
    auto dir = MakeTempDir("reldb_mvcc2");
    lsmkv::DB* kv = OpenTemp(dir);
    expect(kv != nullptr, "open");
    reldb::MvccStore store(kv);
    reldb::Value pk = reldb::Value::Int64(1);

    // t=10: insert ann
    reldb::VersionRecord v1;
    v1.start_ts = 10;
    v1.end_ts = 0;
    v1.prev_ts = 0;
    v1.payload = MakeUser(1, "ann").Encode();
    expect(store.PutVersion("users", pk, v1).ok(), "v1");

    // t=20: update to bob — close v1, write v2
    expect(store.CloseVersion("users", pk, 10, 20).ok(), "close v1");
    reldb::VersionRecord v2;
    v2.start_ts = 20;
    v2.end_ts = 0;
    v2.prev_ts = 10;
    v2.payload = MakeUser(1, "bob").Encode();
    expect(store.PutVersion("users", pk, v2).ok(), "v2");

    reldb::Row row;
    expect(store.GetRow("users", pk, 10, &row).ok(), "s10");
    expect(row.at(1) == reldb::Value::String("ann"), "ann at 10");
    expect(store.GetRow("users", pk, 19, &row).ok(), "s19");
    expect(row.at(1) == reldb::Value::String("ann"), "ann at 19");
    expect(store.GetRow("users", pk, 20, &row).ok(), "s20");
    expect(row.at(1) == reldb::Value::String("bob"), "bob at 20");
    expect(store.GetRow("users", pk, 50, &row).ok(), "s50");
    expect(row.at(1) == reldb::Value::String("bob"), "bob at 50");

    delete kv;
    RemoveDirRecursive(dir);
}

TEST(reldb_mvcc_delete_tombstone) {
    auto dir = MakeTempDir("reldb_mvcc3");
    lsmkv::DB* kv = OpenTemp(dir);
    expect(kv != nullptr, "open");
    reldb::MvccStore store(kv);
    reldb::Value pk = reldb::Value::Int64(7);

    reldb::VersionRecord v1;
    v1.start_ts = 5;
    v1.payload = MakeUser(7, "zoe").Encode();
    expect(store.PutVersion("users", pk, v1).ok(), "insert");

    expect(store.CloseVersion("users", pk, 5, 15).ok(), "close");
    reldb::VersionRecord tomb;
    tomb.start_ts = 15;
    tomb.prev_ts = 5;
    tomb.is_tombstone = true;
    expect(store.PutVersion("users", pk, tomb).ok(), "tomb");

    reldb::Row row;
    expect(store.GetRow("users", pk, 5, &row).ok(), "before delete");
    expect(row.at(1) == reldb::Value::String("zoe"), "zoe");
    expect(store.GetRow("users", pk, 14, &row).ok(), "still live");
    expect(store.GetRow("users", pk, 15, &row).IsNotFound(), "deleted at 15");
    expect(store.GetRow("users", pk, 100, &row).IsNotFound(), "still deleted");

    delete kv;
    RemoveDirRecursive(dir);
}

TEST(reldb_mvcc_missing_row) {
    auto dir = MakeTempDir("reldb_mvcc4");
    lsmkv::DB* kv = OpenTemp(dir);
    expect(kv != nullptr, "open");
    reldb::MvccStore store(kv);
    reldb::Row row;
    expect(store.GetRow("users", reldb::Value::Int64(99), 1, &row).IsNotFound(), "miss");
    std::uint64_t ts = 0;
    expect(store.GetLatestStartTs("users", reldb::Value::Int64(99), &ts).IsNotFound(),
           "no head");
    delete kv;
    RemoveDirRecursive(dir);
}

TEST(reldb_mvcc_persists_across_reopen) {
    auto dir = MakeTempDir("reldb_mvcc5");
    lsmkv::Options opt;
    opt.create_if_missing = true;
    lsmkv::DB* kv = nullptr;
    expect(lsmkv::DB::Open(opt, dir, &kv).ok(), "open");
    {
        reldb::MvccStore store(kv);
        reldb::VersionRecord v;
        v.start_ts = 3;
        v.payload = MakeUser(1, "persist").Encode();
        expect(store.PutVersion("users", reldb::Value::Int64(1), v).ok(), "put");
    }
    delete kv;
    expect(lsmkv::DB::Open(opt, dir, &kv).ok(), "reopen");
    reldb::MvccStore store(kv);
    reldb::Row row;
    expect(store.GetRow("users", reldb::Value::Int64(1), 3, &row).ok(), "get");
    expect(row.at(1) == reldb::Value::String("persist"), "val");
    delete kv;
    RemoveDirRecursive(dir);
}
