#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "lsmkv/status.h"
#include "reldb/schema.h"
#include "reldb/types.h"

namespace reldb {

// One table in a FROM / JOIN list after catalog lookup.
struct BoundTable {
    std::string table_name;
    std::string alias;  // empty if no AS / bare alias
    TableSchema schema;
    int row_offset = 0;  // first column index in a concatenated join row

    // Name used for qualified refs and multi-table SELECT * labels.
    const std::string& CorrelationName() const {
        return alias.empty() ? table_name : alias;
    }
};

// Result of resolving a column ref ("id" or "u.id") against a BindContext.
struct BoundColumn {
    int table_index = -1;
    int column_index = -1;  // within BoundTable::schema
    int row_offset = -1;    // absolute index in a concatenated join row
    ColumnType type = ColumnType::kString;
    std::string column_name;  // bare column name (no qualifier)
};

// Multi-table name resolution for SELECT / WHERE / ON / GROUP BY.
// Unqualified names must match exactly one table; qualified names match
// alias.col or table.col. Duplicate correlation names are rejected at AddTable.
class BindContext {
public:
    // Append a FROM item + its catalog schema. Registers table_name and, when
    // different, alias as qualifier keys (SQLite/MySQL-style: both users.id and
    // u.id work after FROM users AS u). Self-joins of the same table name will
    // need a correlation-only model later; BindContext is built per statement.
    lsmkv::Status AddTable(std::string table_name, std::string alias, TableSchema schema);

    // Resolve "col" or "qual.col". Errors: unknown table/column, ambiguous.
    lsmkv::Status Resolve(const std::string& ref, BoundColumn* out) const;

    // SELECT * output labels: bare names for a single table; correlation.col
    // left-to-right when more than one table is present.
    std::vector<std::string> StarOutputNames() const;

    const std::vector<BoundTable>& tables() const { return tables_; }
    int num_tables() const { return static_cast<int>(tables_.size()); }
    int total_columns() const { return total_columns_; }

    // Split "qual.col" → qual, col; bare "col" → empty qual, col.
    // More than one '.' is InvalidArgument.
    static lsmkv::Status SplitColumnRef(const std::string& ref, std::string* qualifier,
                                        std::string* column);

private:
    int FindTable(const std::string& qualifier) const;

    std::vector<BoundTable> tables_;
    // correlation name (table or alias) → index in tables_
    std::unordered_map<std::string, int> name_to_table_;
    int total_columns_ = 0;
};

}  // namespace reldb
