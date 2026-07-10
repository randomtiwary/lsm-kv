#include <memory>

#include "test_harness.h"
#include "test_util.h"

#include "lsmkv/db.h"
#include "reldb/mvcc.h"
#include "reldb/row.h"
#include "reldb/types.h"

TEST(reldb_mvcc_visibility_basic) {
    reldb::VersionRecord v;
    v.start_ts = 10;
    v.end_ts = 0;

    expect(reldb::IsCommittedVisible(v, 10), "at start");
    expect(reldb::IsCommittedVisible(v, 100), "after start");
    expect(!reldb::IsCommittedVisible(v, 9), "before start");

    v.end_ts = 20;
    expect(reldb::IsCommittedVisible(v, 10), "at start closed");
    expect(reldb::IsCommittedVisible(v, 19), "before end");
    expect(!reldb::IsCommittedVisible(v, 20), "at end exclusive");
    expect(!reldb::IsCommittedVisible(v, 21), "after end");

    v.start_ts = 0;  // provisional
    expect(!reldb::IsCommittedVisible(v, 100), "provisional never committed-visible");
}

TEST(reldb_mvcc_version_record_codec) {
    reldb::VersionRecord v;
    v.version_id = 7;
    v.start_ts = 5;
    v.end_ts = 9;
    v.prev_id = 3;
    v.created_by = 2;
    v.is_tombstone = false;
    v.payload = reldb::Row({reldb::Value::Int64(1), reldb::Value::String("x")}).Encode();

    reldb::VersionRecord out;
    expect(reldb::VersionRecord::Decode(v.Encode(), &out).ok(), "decode");
    expect_eq(out.version_id, static_cast<reldb::Timestamp>(7), "vid");
    expect_eq(out.start_ts, static_cast<reldb::Timestamp>(5), "start");
    expect_eq(out.end_ts, static_cast<reldb::Timestamp>(9), "end");
    expect_eq(out.prev_id, static_cast<reldb::Timestamp>(3), "prev");
    expect_eq(out.created_by, static_cast<reldb::TxnId>(2), "by");
    expect(!out.is_tombstone, "not tomb");
    expect_eq(out.payload, v.payload, "payload");
}

TEST(reldb_mvcc_pk_key_encoding) {
    reldb::Value pk = reldb::Value::String("a/b");
    std::string hex = reldb::EncodePkForKey(pk);
    expect(hex.find('/') == std::string::npos, "no slash in hex");
    reldb::Value back;
    expect(reldb::DecodePkFromKey(hex, &back).ok(), "decode pk");
    expect(back == pk, "pk roundtrip");
}

namespace {

std::shared_ptr<lsmkv::DB> OpenTemp(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    lsmkv::DB* raw = nullptr;
    if (!lsmkv::DB::Open(opt, dir, &raw).ok()) return nullptr;
    return std::shared_ptr<lsmkv::DB>(raw);
}

reldb::Row MakeUser(std::int64_t id, const std::string& name) {
    return reldb::Row({reldb::Value::Int64(id), reldb::Value::String(name)});
}

}  // namespace

TEST(reldb_mvcc_insert_and_read_at_snapshot) {
    auto dir = MakeTempDir("reldb_mvcc1");
    auto kv = OpenTemp(dir);
    expect(kv != nullptr, "open");
    reldb::MvccStore store(kv);

    reldb::Value pk = reldb::Value::Int64(1);
    reldb::VersionRecord v1;
    v1.version_id = 10;
    v1.start_ts = 10;
    v1.created_by = 1;
    v1.payload = MakeUser(1, "ann").Encode();
    expect(store.PutVersion("users", pk, v1).ok(), "put v1");

    reldb::Row row;
    expect(store.GetRowCommitted("users", pk, /*snapshot=*/10, &row).ok(), "snap 10");
    expect(row.at(1) == reldb::Value::String("ann"), "ann");
    expect(store.GetRowCommitted("users", pk, /*snapshot=*/9, &row).IsNotFound(), "snap 9");

    reldb::Timestamp latest = 0;
    expect(store.GetLatestVersionId("users", pk, &latest).ok(), "latest");
    expect_eq(latest, static_cast<reldb::Timestamp>(10), "latest 10");

    RemoveDirRecursive(dir);
}

TEST(reldb_mvcc_update_chain_visibility) {
    auto dir = MakeTempDir("reldb_mvcc2");
    auto kv = OpenTemp(dir);
    expect(kv != nullptr, "open");
    reldb::MvccStore store(kv);
    reldb::Value pk = reldb::Value::Int64(1);

    reldb::VersionRecord v1;
    v1.version_id = 10;
    v1.start_ts = 10;
    v1.created_by = 1;
    v1.payload = MakeUser(1, "ann").Encode();
    expect(store.PutVersion("users", pk, v1).ok(), "v1");

    v1.end_ts = 20;
    expect(store.PutVersionValue("users", pk, v1).ok(), "close v1");

    reldb::VersionRecord v2;
    v2.version_id = 20;
    v2.start_ts = 20;
    v2.prev_id = 10;
    v2.created_by = 2;
    v2.payload = MakeUser(1, "bob").Encode();
    expect(store.PutVersion("users", pk, v2).ok(), "v2");

    reldb::Row row;
    expect(store.GetRowCommitted("users", pk, 10, &row).ok(), "s10");
    expect(row.at(1) == reldb::Value::String("ann"), "ann at 10");
    expect(store.GetRowCommitted("users", pk, 19, &row).ok(), "s19");
    expect(row.at(1) == reldb::Value::String("ann"), "ann at 19");
    expect(store.GetRowCommitted("users", pk, 20, &row).ok(), "s20");
    expect(row.at(1) == reldb::Value::String("bob"), "bob at 20");

    RemoveDirRecursive(dir);
}

TEST(reldb_mvcc_delete_tombstone) {
    auto dir = MakeTempDir("reldb_mvcc3");
    auto kv = OpenTemp(dir);
    expect(kv != nullptr, "open");
    reldb::MvccStore store(kv);
    reldb::Value pk = reldb::Value::Int64(7);

    reldb::VersionRecord v1;
    v1.version_id = 5;
    v1.start_ts = 5;
    v1.created_by = 1;
    v1.payload = MakeUser(7, "zoe").Encode();
    expect(store.PutVersion("users", pk, v1).ok(), "insert");

    v1.end_ts = 15;
    expect(store.PutVersionValue("users", pk, v1).ok(), "close");

    reldb::VersionRecord tomb;
    tomb.version_id = 15;
    tomb.start_ts = 15;
    tomb.prev_id = 5;
    tomb.created_by = 2;
    tomb.is_tombstone = true;
    expect(store.PutVersion("users", pk, tomb).ok(), "tomb");

    reldb::Row row;
    expect(store.GetRowCommitted("users", pk, 5, &row).ok(), "before delete");
    expect(store.GetRowCommitted("users", pk, 15, &row).IsNotFound(), "deleted at 15");

    RemoveDirRecursive(dir);
}

TEST(reldb_mvcc_missing_row) {
    auto dir = MakeTempDir("reldb_mvcc4");
    auto kv = OpenTemp(dir);
    expect(kv != nullptr, "open");
    reldb::MvccStore store(kv);
    reldb::Row row;
    expect(store.GetRowCommitted("users", reldb::Value::Int64(99), 1, &row).IsNotFound(),
           "miss");
    RemoveDirRecursive(dir);
}
