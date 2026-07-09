#pragma once

#include <string>
#include <unordered_map>

#include "lsmkv/db.h"
#include "lsmkv/status.h"
#include "reldb/schema.h"

namespace reldb {

// Persists TableSchema records in an lsmkv::DB under keys:
//   c/t/<table_name>  →  TableSchema::Encode() bytes
//
// Keeps an in-memory cache populated on Create / LoadAll / Get (after miss).
// v1 does not support DROP TABLE or ALTER.
class Catalog {
public:
    explicit Catalog(lsmkv::DB* db);

    // Validate schema, reject duplicates, Put to KV, update cache.
    lsmkv::Status CreateTable(const TableSchema& schema);

    // Lookup by name (cache then KV). NotFound if missing.
    lsmkv::Status GetTable(const std::string& name, TableSchema* out) const;

    bool HasTable(const std::string& name) const;

    // Key encoding helper (also used by tests).
    static std::string TableKey(const std::string& table_name);

private:
    lsmkv::DB* db_;  // not owned
    // Mutable cache: GetTable is logically const from the caller's view.
    mutable std::unordered_map<std::string, TableSchema> cache_;
};

}  // namespace reldb
