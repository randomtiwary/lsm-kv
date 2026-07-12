#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "reldb/expr.h"
#include "reldb/types.h"

namespace reldb {

// Unbound SQL statement ASTs. Expressions reuse reldb::Expr (unbound until Bind).
// Statement is a std::variant; move-only because Select/Update hold unique_ptr.

struct BeginStmt {};
struct CommitStmt {};
struct AbortStmt {};

// One column in CREATE TABLE.
struct ColumnDefAst {
    std::string name;
    ColumnType type = ColumnType::kString;
    bool primary_key = false;
};

struct CreateTableStmt {
    std::string table_name;
    std::vector<ColumnDefAst> columns;
};

// Single-row INSERT.
struct InsertStmt {
    std::string table_name;
    // Empty means column list omitted (INSERT INTO t VALUES (...)).
    std::vector<std::string> column_names;
    std::vector<Value> values;
};

struct OrderByItem {
    std::string column_name;
    bool ascending = true;
};

struct SelectStmt {
    bool select_star = false;
    // Used when select_star is false (column refs or other exprs).
    std::vector<std::unique_ptr<Expr>> select_list;
    std::string table_name;
    std::unique_ptr<Expr> where;  // null if no WHERE
    std::vector<OrderByItem> order_by;
    bool has_limit = false;
    std::int64_t limit = 0;
};

struct AssignmentAst {
    std::string column_name;
    std::unique_ptr<Expr> value;
};

struct UpdateStmt {
    std::string table_name;
    std::vector<AssignmentAst> sets;
    std::unique_ptr<Expr> where;  // null if no WHERE (full table)
};

struct DeleteStmt {
    std::string table_name;
    std::unique_ptr<Expr> where;  // null if no WHERE (full table)
};

using Statement = std::variant<BeginStmt, CommitStmt, AbortStmt, CreateTableStmt, InsertStmt,
                               SelectStmt, UpdateStmt, DeleteStmt>;

// Type predicates for Statement.
inline bool IsBegin(const Statement& s) { return std::holds_alternative<BeginStmt>(s); }
inline bool IsCommit(const Statement& s) { return std::holds_alternative<CommitStmt>(s); }
inline bool IsAbort(const Statement& s) { return std::holds_alternative<AbortStmt>(s); }
inline bool IsCreateTable(const Statement& s) {
    return std::holds_alternative<CreateTableStmt>(s);
}
inline bool IsInsert(const Statement& s) { return std::holds_alternative<InsertStmt>(s); }
inline bool IsSelect(const Statement& s) { return std::holds_alternative<SelectStmt>(s); }
inline bool IsUpdate(const Statement& s) { return std::holds_alternative<UpdateStmt>(s); }
inline bool IsDelete(const Statement& s) { return std::holds_alternative<DeleteStmt>(s); }

// Pretty-print an AST node (single line, stable format for tests).
std::string ToString(const Statement& stmt);

}  // namespace reldb
