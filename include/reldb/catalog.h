#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "lsmkv/db.h"
#include "lsmkv/status.h"
#include "reldb/schema.h"

namespace reldb {

// Persists TableSchema records in an lsmkv::DB under keys:
//   c/t/<table_name>  →  TableSchema::Encode() bytes
//
// Keeps an in-memory cache populated on Create / Get (after miss).
// v1 does not support DROP TABLE or ALTER (see roadmap).
//
// Thread-safety: Catalog itself never locks. Concurrent Create/Get/Has that
// touch the mutable cache_ are data races unless the caller serializes them.
// When Catalog is owned by reldb::Database, every call must be made while
// holding Database::mu_ (use Database::CreateTable / GetTable / HasTable).
// Standalone unit tests may use Catalog single-threaded without a lock.
//
// Shares ownership of the underlying DB via shared_ptr so Catalog can outlive
// the creating stack frame without a raw non-owning pointer.
class Catalog {
public:
    explicit Catalog(std::shared_ptr<lsmkv::DB> db);

    // Validate schema, reject duplicates, Put to KV, update cache.
    // Precondition (multi-threaded): caller holds Database::mu_.
    lsmkv::Status CreateTable(const TableSchema& schema);

    // Lookup by name (cache then KV). NotFound if missing.
    // Precondition (multi-threaded): caller holds Database::mu_.
    lsmkv::Status GetTable(const std::string& name, TableSchema* out) const;

    // Sets *exists to whether the table is present. Returns OK on a definitive
    // answer; propagates IO/corruption (and other) errors from the KV layer.
    // Does not treat non-NotFound failures as "missing".
    // Precondition (multi-threaded): caller holds Database::mu_.
    lsmkv::Status HasTable(const std::string& name, bool* exists) const;

    // Key encoding helper (also used by tests).
    static std::string TableKey(const std::string& table_name);

private:
    std::shared_ptr<lsmkv::DB> db_;
    // Mutable cache: GetTable is logically const from the caller's view.
    mutable std::unordered_map<std::string, TableSchema> cache_;
};

}  // namespace reldb
