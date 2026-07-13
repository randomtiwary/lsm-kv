#include "reldb/catalog.h"

#include "lsmkv/options.h"
#include "reldb/macros.h"

namespace reldb {

Catalog::Catalog(std::shared_ptr<lsmkv::DB> db) : db_(std::move(db)) {}

std::string Catalog::TableKey(const std::string& table_name) {
    return "c/t/" + table_name;
}

bool Catalog::LookupCache(const std::string& name, TableSchema* out) const {
    if (out == nullptr) return false;
    auto it = cache_.find(name);
    if (it == cache_.end()) return false;
    *out = it->second;
    return true;
}

lsmkv::Status Catalog::CreateTable(const TableSchema& schema) {
    RELDB_RETURN_NOT_OK(schema.Validate());

    bool exists = false;
    RELDB_RETURN_NOT_OK(HasTable(schema.name(), &exists));
    if (exists) {
        return STATUS(InvalidArgument, "table already exists: " + schema.name());
    }

    RELDB_RETURN_NOT_OK(
        db_->Put(lsmkv::WriteOptions(), TableKey(schema.name()), schema.Encode()));

    cache_[schema.name()] = schema;
    return STATUS(OK);
}

lsmkv::Status Catalog::DropTable(const std::string& name) {
    bool exists = false;
    RELDB_RETURN_NOT_OK(HasTable(name, &exists));
    if (!exists) {
        return STATUS(NotFound, "table not found: " + name);
    }

    RELDB_RETURN_NOT_OK(db_->Delete(lsmkv::WriteOptions(), TableKey(name)));
    cache_.erase(name);
    return STATUS(OK);
}

lsmkv::Status Catalog::PutTable(const TableSchema& schema) {
    RELDB_RETURN_NOT_OK(schema.Validate());

    RELDB_RETURN_NOT_OK(
        db_->Put(lsmkv::WriteOptions(), TableKey(schema.name()), schema.Encode()));

    cache_[schema.name()] = schema;
    return STATUS(OK);
}

lsmkv::Status Catalog::GetTable(const std::string& name, TableSchema* out) const {
    if (out == nullptr) {
        return STATUS(InvalidArgument, "null out");
    }
    if (LookupCache(name, out)) {
        return STATUS(OK);
    }

    std::string bytes;
    auto st = db_->Get(lsmkv::ReadOptions(), TableKey(name), &bytes);
    if (st.IsNotFound()) {
        return STATUS(NotFound, "table not found: " + name);
    }
    RELDB_RETURN_NOT_OK(st);

    TableSchema schema;
    RELDB_RETURN_NOT_OK(TableSchema::Decode(bytes, &schema));
    cache_[name] = schema;
    *out = schema;
    return STATUS(OK);
}

lsmkv::Status Catalog::HasTable(const std::string& name, bool* exists) const {
    if (exists == nullptr) {
        return STATUS(InvalidArgument, "null exists");
    }
    TableSchema unused;
    auto st = GetTable(name, &unused);
    if (st.ok()) {
        *exists = true;
        return STATUS(OK);
    }
    if (st.IsNotFound()) {
        *exists = false;
        return STATUS(OK);
    }
    // IO / corruption / etc. — not a definitive "missing".
    return st;
}

}  // namespace reldb
