#include <memory>

#include "test_harness.h"
#include "test_util.h"

#include "lsmkv/db.h"
#include "reldb/catalog.h"
#include "reldb/schema.h"
#include "reldb/types.h"

namespace reldb {

// Test-only peer for Catalog private methods (single-threaded unit tests).
class CatalogTestAccess {
public:
    static lsmkv::Status CreateTable(Catalog& cat, const TableSchema& schema) {
        return cat.CreateTable(schema);
    }
    static lsmkv::Status GetTable(const Catalog& cat, const std::string& name,
                                  TableSchema* out) {
        return cat.GetTable(name, out);
    }
    static lsmkv::Status HasTable(const Catalog& cat, const std::string& name, bool* exists) {
        return cat.HasTable(name, exists);
    }
};

}  // namespace reldb

namespace {

reldb::TableSchema UsersSchema() {
    return reldb::TableSchema("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
    });
}

// Open a DB and wrap it in a shared_ptr so tests never call delete.
std::shared_ptr<lsmkv::DB> OpenKv(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    lsmkv::DB* raw = nullptr;
    if (!lsmkv::DB::Open(opt, dir, &raw).ok()) {
        return nullptr;
    }
    return std::shared_ptr<lsmkv::DB>(raw);
}

using Access = reldb::CatalogTestAccess;

}  // namespace

TEST(reldb_catalog_create_and_get) {
    auto dir = MakeTempDir("reldb_cat");
    auto kv = OpenKv(dir);
    expect(kv != nullptr, "open kv");

    reldb::Catalog cat(kv);
    expect(Access::CreateTable(cat, UsersSchema()).ok(), "create");
    bool exists = false;
    expect(Access::HasTable(cat, "users", &exists).ok(), "has status");
    expect(exists, "has");
    exists = true;
    expect(Access::HasTable(cat, "missing", &exists).ok(), "missing status");
    expect(!exists, "missing");

    reldb::TableSchema got;
    expect(Access::GetTable(cat, "users", &got).ok(), "get");
    expect_eq(got.name(), std::string("users"), "name");
    expect_eq(got.num_columns(), static_cast<std::size_t>(2), "ncols");
    expect(got.columns()[0].primary_key, "pk");

    expect(Access::GetTable(cat, "missing", &got).IsNotFound(), "missing get");

    kv.reset();
    RemoveDirRecursive(dir);
}

TEST(reldb_catalog_reject_duplicate_and_invalid) {
    auto dir = MakeTempDir("reldb_cat2");
    auto kv = OpenKv(dir);
    expect(kv != nullptr, "open");

    reldb::Catalog cat(kv);
    expect(Access::CreateTable(cat, UsersSchema()).ok(), "create");
    expect(Access::CreateTable(cat, UsersSchema()).IsInvalidArgument(), "dup");

    reldb::TableSchema bad("", {{"id", reldb::ColumnType::kInt64, true}});
    expect(Access::CreateTable(cat, bad).IsInvalidArgument(), "invalid schema");

    kv.reset();
    RemoveDirRecursive(dir);
}

TEST(reldb_catalog_persists_across_reopen) {
    auto dir = MakeTempDir("reldb_cat3");
    {
        auto kv = OpenKv(dir);
        expect(kv != nullptr, "open");
        reldb::Catalog cat(kv);
        expect(Access::CreateTable(cat, UsersSchema()).ok(), "create");
        reldb::TableSchema accounts("accounts", {
            {"uid", reldb::ColumnType::kString, true},
            {"bal", reldb::ColumnType::kInt64, false},
        });
        expect(Access::CreateTable(cat, accounts).ok(), "create accounts");
    }

    auto kv = OpenKv(dir);
    expect(kv != nullptr, "reopen");
    reldb::Catalog cat2(kv);
    reldb::TableSchema got;
    expect(Access::GetTable(cat2, "users", &got).ok(), "users after reopen");
    expect_eq(got.columns()[1].name, std::string("name"), "col");
    expect(Access::GetTable(cat2, "accounts", &got).ok(), "accounts after reopen");
    expect(got.columns()[0].type == reldb::ColumnType::kString, "uid type");
    bool exists = false;
    expect(Access::HasTable(cat2, "users", &exists).ok(), "has status after reopen");
    expect(exists, "has after reopen");

    // Key layout is stable for debugging / future tools.
    expect_eq(reldb::Catalog::TableKey("users"), std::string("c/t/users"), "key");

    kv.reset();
    RemoveDirRecursive(dir);
}
