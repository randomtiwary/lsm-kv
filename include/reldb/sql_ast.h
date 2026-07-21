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

struct DropTableStmt {
    std::string table_name;
    // When true (DROP TABLE IF EXISTS), a missing table is not an error.
    bool if_exists = false;
};

// ALTER TABLE name ADD COLUMN col type DEFAULT literal (no PRIMARY KEY).
struct AlterTableAddColumnStmt {
    std::string table_name;
    ColumnDefAst column;  // primary_key always false from the parser
    Value default_value;
};

// ALTER TABLE name DROP COLUMN col (cannot drop PK — enforced at Database).
struct AlterTableDropColumnStmt {
    std::string table_name;
    std::string column_name;
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

// Aggregate function names used by SelectItem (parser/binder/execution).
enum class AggFunc : std::uint8_t {
    kCount = 0,
    kSum = 1,
    kAvg = 2,
    kMin = 3,
    kMax = 4,
};

// Shared naming for aggregate result columns (parser HAVING rewrite + session).
const char* AggFuncName(AggFunc f);
// e.g. COUNT(*), SUM(score) — no AS alias.
std::string DefaultAggResultName(AggFunc f, bool star, const std::string& col);
// Inverse of DefaultAggResultName; returns false if name is not that form.
bool ParseDefaultAggResultName(const std::string& name, AggFunc* f, bool* star,
                               std::string* col);

// One SELECT list item: a plain expression or an aggregate (COUNT/SUM/…).
// Aggregates and output_name are filled when the parser/binder support them.
struct SelectItem {
    enum class Kind : std::uint8_t { kExpr = 0, kAgg = 1 };

    Kind kind = Kind::kExpr;
    // kExpr: projection expression.
    std::unique_ptr<Expr> expr;
    // kAgg: function, COUNT(*) vs column argument.
    AggFunc agg_func = AggFunc::kCount;
    bool agg_star = false;       // COUNT(*)
    std::string agg_column;      // column ref when !agg_star
    std::string alias;           // optional AS name
    std::string output_name;     // result column name after binding
};

// FROM clause. Currently a single table; joins can extend this later.
struct FromClause {
    std::string table_name;
};

struct SelectStmt {
    bool select_star = false;
    // Used when select_star is false.
    std::vector<SelectItem> select_list;
    FromClause from;
    std::unique_ptr<Expr> where;  // null if no WHERE
    // Empty when the query has no GROUP BY / HAVING.
    std::vector<std::string> group_by;
    std::unique_ptr<Expr> having;
    std::vector<OrderByItem> order_by;
    bool has_limit = false;
    std::int64_t limit = 0;
};

// Helper: wrap a projection expression as a SelectItem.
inline SelectItem MakeExprSelectItem(std::unique_ptr<Expr> expr) {
    SelectItem item;
    item.kind = SelectItem::Kind::kExpr;
    item.expr = std::move(expr);
    return item;
}

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

using Statement =
    std::variant<BeginStmt, CommitStmt, AbortStmt, CreateTableStmt, DropTableStmt,
                 AlterTableAddColumnStmt, AlterTableDropColumnStmt, InsertStmt, SelectStmt,
                 UpdateStmt, DeleteStmt>;

// Type predicates for Statement.
inline bool IsBegin(const Statement& s) { return std::holds_alternative<BeginStmt>(s); }
inline bool IsCommit(const Statement& s) { return std::holds_alternative<CommitStmt>(s); }
inline bool IsAbort(const Statement& s) { return std::holds_alternative<AbortStmt>(s); }
inline bool IsCreateTable(const Statement& s) {
    return std::holds_alternative<CreateTableStmt>(s);
}
inline bool IsDropTable(const Statement& s) {
    return std::holds_alternative<DropTableStmt>(s);
}
inline bool IsAlterTableAddColumn(const Statement& s) {
    return std::holds_alternative<AlterTableAddColumnStmt>(s);
}
inline bool IsAlterTableDropColumn(const Statement& s) {
    return std::holds_alternative<AlterTableDropColumnStmt>(s);
}
inline bool IsInsert(const Statement& s) { return std::holds_alternative<InsertStmt>(s); }
inline bool IsSelect(const Statement& s) { return std::holds_alternative<SelectStmt>(s); }
inline bool IsUpdate(const Statement& s) { return std::holds_alternative<UpdateStmt>(s); }
inline bool IsDelete(const Statement& s) { return std::holds_alternative<DeleteStmt>(s); }

// Pretty-print an AST node (single line, stable format for tests).
std::string ToString(const Statement& stmt);

}  // namespace reldb
