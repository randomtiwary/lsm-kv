#include "test_harness.h"
#include "test_util.h"

#include "lsmkv/db.h"
#include "reldb/catalog.h"
#include "reldb/schema.h"
#include "reldb/types.h"

namespace {

reldb::TableSchema UsersSchema() {
    return reldb::TableSchema("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
    });
}

}  // namespace

TEST(reldb_catalog_create_and_get) {
    auto dir = MakeTempDir("reldb_cat");
    lsmkv::Options opt;
    opt.create_if_missing = true;
    lsmkv::DB* kv = nullptr;
    expect(lsmkv::DB::Open(opt, dir, &kv).ok(), "open kv");

    reldb::Catalog cat(kv);
    expect(cat.CreateTable(UsersSchema()).ok(), "create");
    expect(cat.HasTable("users"), "has");

    reldb::TableSchema got;
    expect(cat.GetTable("users", &got).ok(), "get");
    expect_eq(got.name(), std::string("users"), "name");
    expect_eq(got.num_columns(), static_cast<std::size_t>(2), "ncols");
    expect(got.columns()[0].primary_key, "pk");

    expect(cat.GetTable("missing", &got).IsNotFound(), "missing");

    delete kv;
    RemoveDirRecursive(dir);
}

TEST(reldb_catalog_reject_duplicate_and_invalid) {
    auto dir = MakeTempDir("reldb_cat2");
    lsmkv::Options opt;
    opt.create_if_missing = true;
    lsmkv::DB* kv = nullptr;
    expect(lsmkv::DB::Open(opt, dir, &kv).ok(), "open");

    reldb::Catalog cat(kv);
    expect(cat.CreateTable(UsersSchema()).ok(), "create");
    expect(cat.CreateTable(UsersSchema()).IsInvalidArgument(), "dup");

    reldb::TableSchema bad("", {{"id", reldb::ColumnType::kInt64, true}});
    expect(cat.CreateTable(bad).IsInvalidArgument(), "invalid schema");

    delete kv;
    RemoveDirRecursive(dir);
}

TEST(reldb_catalog_persists_across_reopen) {
    auto dir = MakeTempDir("reldb_cat3");
    lsmkv::Options opt;
    opt.create_if_missing = true;
    lsmkv::DB* kv = nullptr;
    expect(lsmkv::DB::Open(opt, dir, &kv).ok(), "open");

    {
        reldb::Catalog cat(kv);
        expect(cat.CreateTable(UsersSchema()).ok(), "create");
        reldb::TableSchema accounts("accounts", {
            {"uid", reldb::ColumnType::kString, true},
            {"bal", reldb::ColumnType::kInt64, false},
        });
        expect(cat.CreateTable(accounts).ok(), "create accounts");
    }
    delete kv;

    expect(lsmkv::DB::Open(opt, dir, &kv).ok(), "reopen");
    reldb::Catalog cat2(kv);
    reldb::TableSchema got;
    expect(cat2.GetTable("users", &got).ok(), "users after reopen");
    expect_eq(got.columns()[1].name, std::string("name"), "col");
    expect(cat2.GetTable("accounts", &got).ok(), "accounts after reopen");
    expect(got.columns()[0].type == reldb::ColumnType::kString, "uid type");
    expect(cat2.HasTable("users"), "has after reopen");

    // Key layout is stable for debugging / future tools.
    expect_eq(reldb::Catalog::TableKey("users"), std::string("c/t/users"), "key");

    delete kv;
    RemoveDirRecursive(dir);
}
