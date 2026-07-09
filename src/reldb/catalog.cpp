#include "reldb/catalog.h"

#include "lsmkv/options.h"

namespace reldb {

Catalog::Catalog(lsmkv::DB* db) : db_(db) {}

std::string Catalog::TableKey(const std::string& table_name) {
    return "c/t/" + table_name;
}

lsmkv::Status Catalog::CreateTable(const TableSchema& schema) {
    auto st = schema.Validate();
    if (!st.ok()) return st;

    bool exists = false;
    st = HasTable(schema.name(), &exists);
    if (!st.ok()) return st;
    if (exists) {
        return lsmkv::Status::InvalidArgument("table already exists: " + schema.name());
    }

    st = db_->Put(lsmkv::WriteOptions(), TableKey(schema.name()), schema.Encode());
    if (!st.ok()) return st;

    cache_[schema.name()] = schema;
    return lsmkv::Status::OK();
}

lsmkv::Status Catalog::GetTable(const std::string& name, TableSchema* out) const {
    if (out == nullptr) {
        return lsmkv::Status::InvalidArgument("null out");
    }
    auto it = cache_.find(name);
    if (it != cache_.end()) {
        *out = it->second;
        return lsmkv::Status::OK();
    }

    std::string bytes;
    auto st = db_->Get(lsmkv::ReadOptions(), TableKey(name), &bytes);
    if (st.IsNotFound()) {
        return lsmkv::Status::NotFound("table not found: " + name);
    }
    if (!st.ok()) return st;

    TableSchema schema;
    st = TableSchema::Decode(bytes, &schema);
    if (!st.ok()) return st;
    cache_[name] = schema;
    *out = schema;
    return lsmkv::Status::OK();
}

lsmkv::Status Catalog::HasTable(const std::string& name, bool* exists) const {
    if (exists == nullptr) {
        return lsmkv::Status::InvalidArgument("null exists");
    }
    TableSchema unused;
    auto st = GetTable(name, &unused);
    if (st.ok()) {
        *exists = true;
        return lsmkv::Status::OK();
    }
    if (st.IsNotFound()) {
        *exists = false;
        return lsmkv::Status::OK();
    }
    // IO / corruption / etc. — not a definitive "missing".
    return st;
}

}  // namespace reldb
