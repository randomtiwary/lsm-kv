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
    static lsmkv::Status DropTable(Catalog& cat, const std::string& name) {
        return cat.DropTable(name);
    }
    static lsmkv::Status PutTable(Catalog& cat, const TableSchema& schema) {
        return cat.PutTable(schema);
    }
    static lsmkv::Status GetTable(const Catalog& cat, const std::string& name,
                                  TableSchema* out) {
        return cat.GetTable(name, out);
    }
    static lsmkv::Status HasTable(const Catalog& cat, const std::string& name, bool* exists) {
        return cat.HasTable(name, exists);
    }
    // Exposes cache probe for drop/put cache-coherence checks.
    static bool LookupCache(const Catalog& cat, const std::string& name, TableSchema* out) {
        return cat.LookupCache(name, out);
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

TEST(reldb_catalog_drop_table) {
    auto dir = MakeTempDir("reldb_cat_drop");
    auto kv = OpenKv(dir);
    expect(kv != nullptr, "open");

    reldb::Catalog cat(kv);
    expect(Access::CreateTable(cat, UsersSchema()).ok(), "create");

    // Drop removes KV entry and cache.
    expect(Access::DropTable(cat, "users").ok(), "drop");
    bool exists = true;
    expect(Access::HasTable(cat, "users", &exists).ok(), "has after drop");
    expect(!exists, "gone");
    reldb::TableSchema got;
    expect(Access::GetTable(cat, "users", &got).IsNotFound(), "get after drop");
    expect(!Access::LookupCache(cat, "users", &got), "cache cleared");

    // Idempotent failure: missing table is NotFound.
    expect(Access::DropTable(cat, "users").IsNotFound(), "drop missing");
    expect(Access::DropTable(cat, "never_existed").IsNotFound(), "drop never");

    // Create is allowed again after drop.
    expect(Access::CreateTable(cat, UsersSchema()).ok(), "recreate");
    expect(Access::GetTable(cat, "users", &got).ok(), "get recreated");

    kv.reset();
    RemoveDirRecursive(dir);
}

TEST(reldb_catalog_drop_persists) {
    auto dir = MakeTempDir("reldb_cat_drop_persist");
    {
        auto kv = OpenKv(dir);
        expect(kv != nullptr, "open");
        reldb::Catalog cat(kv);
        expect(Access::CreateTable(cat, UsersSchema()).ok(), "create");
        expect(Access::DropTable(cat, "users").ok(), "drop");
    }

    auto kv = OpenKv(dir);
    expect(kv != nullptr, "reopen");
    reldb::Catalog cat2(kv);
    reldb::TableSchema got;
    expect(Access::GetTable(cat2, "users", &got).IsNotFound(), "still gone");
    bool exists = true;
    expect(Access::HasTable(cat2, "users", &exists).ok(), "has status");
    expect(!exists, "has false");

    kv.reset();
    RemoveDirRecursive(dir);
}

TEST(reldb_catalog_put_table) {
    auto dir = MakeTempDir("reldb_cat_put");
    auto kv = OpenKv(dir);
    expect(kv != nullptr, "open");

    reldb::Catalog cat(kv);
    expect(Access::CreateTable(cat, UsersSchema()).ok(), "create");

    // Overwrite schema (simulates ALTER ADD COLUMN catalog step).
    reldb::TableSchema widened("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
        {"age", reldb::ColumnType::kInt64, false},
    });
    expect(Access::PutTable(cat, widened).ok(), "put");

    reldb::TableSchema got;
    expect(Access::GetTable(cat, "users", &got).ok(), "get after put");
    expect_eq(got.num_columns(), static_cast<std::size_t>(3), "ncols");
    expect_eq(got.columns()[2].name, std::string("age"), "new col");

    // Cache must reflect the overwrite (not stale CreateTable entry).
    reldb::TableSchema cached;
    expect(Access::LookupCache(cat, "users", &cached), "cache hit");
    expect_eq(cached.num_columns(), static_cast<std::size_t>(3), "cache ncols");

    // PutTable rejects invalid schemas.
    reldb::TableSchema bad("", {{"id", reldb::ColumnType::kInt64, true}});
    expect(Access::PutTable(cat, bad).IsInvalidArgument(), "invalid put");

    // PutTable can install a brand-new catalog entry (upsert).
    reldb::TableSchema accounts("accounts", {
        {"uid", reldb::ColumnType::kString, true},
        {"bal", reldb::ColumnType::kInt64, false},
    });
    expect(Access::PutTable(cat, accounts).ok(), "put new");
    expect(Access::GetTable(cat, "accounts", &got).ok(), "get accounts");

    kv.reset();
    RemoveDirRecursive(dir);
}

TEST(reldb_catalog_put_persists) {
    auto dir = MakeTempDir("reldb_cat_put_persist");
    {
        auto kv = OpenKv(dir);
        expect(kv != nullptr, "open");
        reldb::Catalog cat(kv);
        expect(Access::CreateTable(cat, UsersSchema()).ok(), "create");
        reldb::TableSchema widened("users", {
            {"id", reldb::ColumnType::kInt64, true},
            {"name", reldb::ColumnType::kString, false},
            {"age", reldb::ColumnType::kInt64, false},
        });
        expect(Access::PutTable(cat, widened).ok(), "put");
    }

    auto kv = OpenKv(dir);
    expect(kv != nullptr, "reopen");
    reldb::Catalog cat2(kv);
    reldb::TableSchema got;
    expect(Access::GetTable(cat2, "users", &got).ok(), "get after reopen");
    expect_eq(got.num_columns(), static_cast<std::size_t>(3), "ncols after reopen");
    expect_eq(got.columns()[2].name, std::string("age"), "age after reopen");

    kv.reset();
    RemoveDirRecursive(dir);
}
