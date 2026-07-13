#pragma once

// Shared pretty-printing for QueryResult (shell + network CLI).

#include <ostream>

#include "reldb/query_result.h"

namespace reldb {

// Print plan_tag, ASCII table (or ok / rows_affected), and row count to out.
// Used by reldb_sql_shell and reldb_sql_cli to avoid duplicated formatting.
void FormatQueryResult(std::ostream& out, const QueryResult& r);

}  // namespace reldb
