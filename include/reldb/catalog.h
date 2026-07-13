#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "lsmkv/db.h"
#include "lsmkv/status.h"
#include "reldb/schema.h"

namespace reldb {

class Database;
// Test-only peer (see tests/test_reldb_catalog.cpp). Not for production use.
class CatalogTestAccess;

// Persists TableSchema records in an lsmkv::DB under keys:
//   c/t/<table_name>  →  TableSchema::Encode() bytes
//
// Keeps an in-memory cache populated on Create / Get (after miss),
// updated by PutTable, and erased by DropTable.
//
// Access control: Create/Drop/Put/Get/Has are private. Only Database
// (production) and CatalogTestAccess (unit tests) may call them. Database
// serializes access with its shared_mutex (shared for pure cache hits, unique
// for mutations / KV loads). Catalog itself never locks.
//
// Shares ownership of the underlying DB via shared_ptr so Catalog can outlive
// the creating stack frame without a raw non-owning pointer.
class Catalog {
public:
    explicit Catalog(std::shared_ptr<lsmkv::DB> db);

    // Key encoding helper (also used by tests).
    static std::string TableKey(const std::string& table_name);

private:
    friend class Database;
    friend class CatalogTestAccess;

    // Validate schema, reject duplicates, Put to KV, update cache.
    // Caller must hold Database::mu_ exclusively (unique_lock).
    lsmkv::Status CreateTable(const TableSchema& schema);

    // Delete catalog key and erase cache entry. NotFound if the table is
    // missing. Does not touch row/version data (Database::DropTable owns GC).
    // Caller must hold Database::mu_ exclusively.
    lsmkv::Status DropTable(const std::string& name);

    // Validate schema, overwrite KV encode, refresh cache. Used by ALTER
    // (and any path that must replace an existing catalog entry). Does not
    // require the table to already exist (upsert of the catalog record).
    // Caller must hold Database::mu_ exclusively.
    lsmkv::Status PutTable(const TableSchema& schema);

    // Lookup by name (cache then KV). NotFound if missing.
    // May mutate cache_ on miss — caller must hold Database::mu_ exclusively.
    lsmkv::Status GetTable(const std::string& name, TableSchema* out) const;

    // Sets *exists to whether the table is present. Returns OK on a definitive
    // answer; propagates IO/corruption (and other) errors from the KV layer.
    // Does not treat non-NotFound failures as "missing".
    // May mutate cache_ — caller must hold Database::mu_ exclusively.
    lsmkv::Status HasTable(const std::string& name, bool* exists) const;

    // Read-only cache probe. Does not touch KV or mutate cache_.
    // Safe under a shared lock on Database::mu_.
    // Returns true and fills *out on hit; false on miss (*out unchanged).
    bool LookupCache(const std::string& name, TableSchema* out) const;

    std::shared_ptr<lsmkv::DB> db_;
    // Mutable cache: GetTable is logically const from the caller's view.
    mutable std::unordered_map<std::string, TableSchema> cache_;
};

}  // namespace reldb
