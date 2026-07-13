#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "lsmkv/status.h"
#include "reldb/types.h"

namespace reldb {

struct ColumnDef {
    std::string name;
    ColumnType type = ColumnType::kString;
    bool primary_key = false;
};

// Table definition: ordered columns, exactly one primary key in v1.
class TableSchema {
public:
    TableSchema() = default;
    TableSchema(std::string name, std::vector<ColumnDef> columns)
        : name_(std::move(name)), columns_(std::move(columns)) {}

    const std::string& name() const { return name_; }
    const std::vector<ColumnDef>& columns() const { return columns_; }
    std::size_t num_columns() const { return columns_.size(); }

    // Index of the primary-key column, or -1 if none / invalid.
    int primary_key_index() const;

    const ColumnDef* FindColumn(const std::string& name) const;
    int ColumnIndex(const std::string& name) const;

    // Validate: non-empty name, >=1 column, unique names, exactly one PK,
    // PK type is not Null, no Null-typed columns.
    lsmkv::Status Validate() const;

    // Binary encode / decode for catalog persistence.
    // Format v1: u8 version=1 | name | ncols | (col_name, type, pk, flags)*.
    std::string Encode() const;
    static lsmkv::Status Decode(const std::string& bytes, TableSchema* out);

private:
    std::string name_;
    std::vector<ColumnDef> columns_;
};

}  // namespace reldb
