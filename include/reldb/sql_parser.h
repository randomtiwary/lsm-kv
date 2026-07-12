#pragma once

#include <string_view>
#include <vector>

#include "lsmkv/status.h"
#include "reldb/sql_ast.h"

namespace reldb {

// Hand-written recursive-descent SQL parser for the dialect in docs/SQL.md.
//
//   ParseScript("BEGIN; SELECT * FROM t WHERE id = 1; COMMIT;", &stmts);
//
// Keywords are case-insensitive. String literals use single quotes ('' escapes).
// Unsupported constructs (JOIN, GROUP BY, subqueries, …) fail with InvalidArgument.
// Produces unbound Statement ASTs; does not bind names or execute.

// Parse a full script: statement (';' statement)* ';'?
// Empty / whitespace-only input yields an empty vector and OK.
lsmkv::Status ParseScript(std::string_view sql, std::vector<Statement>* out);

// Parse exactly one statement (optional trailing ';'). Extra tokens → error.
lsmkv::Status ParseStatement(std::string_view sql, Statement* out);

}  // namespace reldb
