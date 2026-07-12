#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "reldb/row.h"

namespace reldb {

// Result of executing a statement (SELECT fills rows; DML sets rows_affected).
struct QueryResult {
    std::vector<std::string> column_names;
    std::vector<Row> rows;
    std::uint64_t rows_affected = 0;
    // Optional plan label for tests / EXPLAIN (e.g. "PkPointGet").
    std::string plan_tag;

    void Clear() {
        column_names.clear();
        rows.clear();
        rows_affected = 0;
        plan_tag.clear();
    }

    bool empty() const { return rows.empty() && rows_affected == 0; }
};

}  // namespace reldb
