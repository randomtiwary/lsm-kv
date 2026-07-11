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

enum class CmpOp : std::uint8_t {
    kEq = 0,
    kNe,
    kLt,
    kLe,
    kGt,
    kGe,
};

enum class LogicOp : std::uint8_t {
    kAnd = 0,
    kOr,
    kNot,
};

// Expression tree for filters/projections (bound against a TableSchema).
// Owned via unique_ptr; leaves are literals or column references.
class Expr {
public:
    enum class Kind : std::uint8_t {
        kLiteral = 0,
        kColumn,
        kCompare,
        kLogic,
    };

    static std::unique_ptr<Expr> Literal(Value v);
    // Unbound column name; call Bind() before Eval (or Eval binds by name each time
    // if index is still -1).
    static std::unique_ptr<Expr> Column(std::string name);
    static std::unique_ptr<Expr> Compare(CmpOp op, std::unique_ptr<Expr> left,
                                        std::unique_ptr<Expr> right);
    static std::unique_ptr<Expr> And(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right);
    static std::unique_ptr<Expr> Or(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right);
    static std::unique_ptr<Expr> Not(std::unique_ptr<Expr> child);

    Kind kind() const { return kind_; }

    // Resolve column names to indices for schema. Safe to call more than once.
    lsmkv::Status Bind(const TableSchema& schema);

    // Evaluate to a Value (including Bool for predicates, Null for unknown).
    lsmkv::Status Eval(const Row& row, const TableSchema& schema, Value* out) const;

    // For WHERE: Null and non-Bool results are treated as false.
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
