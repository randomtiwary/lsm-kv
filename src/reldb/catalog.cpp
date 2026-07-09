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

    if (HasTable(schema.name())) {
        return lsmkv::Status::InvalidArgument("table already exists: " + schema.name());
    }

    // Double-check KV in case cache is cold after reopen.
    std::string existing;
    st = db_->Get(lsmkv::ReadOptions(), TableKey(schema.name()), &existing);
    if (st.ok()) {
        return lsmkv::Status::InvalidArgument("table already exists: " + schema.name());
    }
    if (!st.IsNotFound()) return st;

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

bool Catalog::HasTable(const std::string& name) const {
    TableSchema unused;
    return GetTable(name, &unused).ok();
}

}  // namespace reldb
