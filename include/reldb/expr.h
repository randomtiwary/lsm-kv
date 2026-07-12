#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lsmkv/status.h"
#include "reldb/row.h"
#include "reldb/schema.h"
#include "reldb/types.h"

namespace reldb {

// Comparison operators used by Compare nodes.
enum class CmpOp : std::uint8_t {
    kEq = 0,  // `=`
    kNe,      // `!=` / `<>`
    kLt,      // `<`
    kLe,      // `<=`
    kGt,      // `>`
    kGe,      // `>=`
};

// Boolean connectives used by Logic nodes.
enum class LogicOp : std::uint8_t {
    kAnd = 0,  // `AND` (binary)
    kOr,       // `OR` (binary)
    kNot,      // `NOT` (unary; child in left_)
};

// Expression tree for filters/projections (bound against a TableSchema).
// Owned via unique_ptr; leaves are literals or column references.
//
// Lifecycle (Bind → Eval):
//   1. Build the tree with factory helpers (Literal / Column / Compare / And / Or / Not).
//      Column nodes store a name only; column_index_ is -1 (unbound).
//   2. Call Bind(schema) once (or after the schema is known). This walks the tree and
//      sets column_index_ for every Column leaf via TableSchema::ColumnIndex. Missing
//      names return InvalidArgument. Bind is idempotent.
//   3. Call Eval(row, schema, &value) on any row that matches that schema width:
//        - Literal → copies the stored Value
//        - Column  → row.at(column_index_) (falls back to name lookup if still unbound)
//        - Compare → eval children, then type-checked comparison (NULL if either side NULL)
//        - Logic   → three-valued AND/OR/NOT (NULL propagates per SQL-style rules)
//   4. For WHERE-style use, call EvalBool: runs Eval; NULL → false (row filtered out);
//      non-Bool → InvalidArgument (bad predicate, e.g. bare column of type INT).
//
// Typical path: Bind once on a planned filter, then EvalBool per scanned row.
class Expr {
public:
    // Node shape in the expression tree.
    enum class Kind : std::uint8_t {
        kLiteral = 0,  // constant Value
        kColumn,       // column reference (name / bound index)
        kCompare,      // left CmpOp right
        kLogic,        // AND / OR / NOT over child expr(s)
    };

    static std::unique_ptr<Expr> Literal(Value v);
    // Unbound column name; call Bind() before Eval for the fast path.
    static std::unique_ptr<Expr> Column(std::string name);
    static std::unique_ptr<Expr> Compare(CmpOp op, std::unique_ptr<Expr> left,
                                        std::unique_ptr<Expr> right);
    static std::unique_ptr<Expr> And(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right);
    static std::unique_ptr<Expr> Or(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right);
    static std::unique_ptr<Expr> Not(std::unique_ptr<Expr> child);

    Kind kind() const { return kind_; }

    // Human-readable tree for tests, debugging, and EXPLAIN-style output.
    // Example: And(Compare(Eq, Column(id), Literal(1)), Column(active))
    std::string ToString() const;

    // Resolve column names to indices for schema. Safe to call more than once.
    lsmkv::Status Bind(const TableSchema& schema);

    // Evaluate to a Value (including Bool for predicates, Null for unknown).
    lsmkv::Status Eval(const Row& row, const TableSchema& schema, Value* out) const;

    // For WHERE: Null → false; non-Bool → InvalidArgument.
    lsmkv::Status EvalBool(const Row& row, const TableSchema& schema, bool* out) const;

private:
    Expr() = default;

    lsmkv::Status EvalCompare(const Row& row, const TableSchema& schema, Value* out) const;
    lsmkv::Status EvalLogic(const Row& row, const TableSchema& schema, Value* out) const;
    static lsmkv::Status CompareValues(CmpOp op, const Value& left, const Value& right,
                                       Value* out);

    Kind kind_ = Kind::kLiteral;
    Value literal_;
    std::string column_name_;
    int column_index_ = -1;  // set by Bind
    CmpOp cmp_op_ = CmpOp::kEq;
    LogicOp logic_op_ = LogicOp::kAnd;
    std::unique_ptr<Expr> left_;
    std::unique_ptr<Expr> right_;  // unused for Not
};

}  // namespace reldb
