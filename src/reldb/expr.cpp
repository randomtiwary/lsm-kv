#include "reldb/expr.h"

#include "reldb/macros.h"

namespace reldb {

std::unique_ptr<Expr> Expr::Literal(Value v) {
    auto e = std::unique_ptr<Expr>(new Expr());
    e->kind_ = Kind::kLiteral;
    e->literal_ = std::move(v);
    return e;
}

std::unique_ptr<Expr> Expr::Column(std::string name) {
    auto e = std::unique_ptr<Expr>(new Expr());
    e->kind_ = Kind::kColumn;
    e->column_name_ = std::move(name);
    e->column_index_ = -1;
    return e;
}

std::unique_ptr<Expr> Expr::Compare(CmpOp op, std::unique_ptr<Expr> left,
                                    std::unique_ptr<Expr> right) {
    auto e = std::unique_ptr<Expr>(new Expr());
    e->kind_ = Kind::kCompare;
    e->cmp_op_ = op;
    e->left_ = std::move(left);
    e->right_ = std::move(right);
    return e;
}

std::unique_ptr<Expr> Expr::And(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right) {
    auto e = std::unique_ptr<Expr>(new Expr());
    e->kind_ = Kind::kLogic;
    e->logic_op_ = LogicOp::kAnd;
    e->left_ = std::move(left);
    e->right_ = std::move(right);
    return e;
}

std::unique_ptr<Expr> Expr::Or(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right) {
    auto e = std::unique_ptr<Expr>(new Expr());
    e->kind_ = Kind::kLogic;
    e->logic_op_ = LogicOp::kOr;
    e->left_ = std::move(left);
    e->right_ = std::move(right);
    return e;
}

std::unique_ptr<Expr> Expr::Not(std::unique_ptr<Expr> child) {
    auto e = std::unique_ptr<Expr>(new Expr());
    e->kind_ = Kind::kLogic;
    e->logic_op_ = LogicOp::kNot;
    e->left_ = std::move(child);
    return e;
}

namespace {

const char* CmpOpName(CmpOp op) {
    switch (op) {
        case CmpOp::kEq: return "Eq";
        case CmpOp::kNe: return "Ne";
        case CmpOp::kLt: return "Lt";
        case CmpOp::kLe: return "Le";
        case CmpOp::kGt: return "Gt";
        case CmpOp::kGe: return "Ge";
    }
    return "?";
}

const char* LogicOpName(LogicOp op) {
    switch (op) {
        case LogicOp::kAnd: return "And";
        case LogicOp::kOr: return "Or";
        case LogicOp::kNot: return "Not";
    }
    return "?";
}

// Quote a Value for Expr::ToString (strings use single quotes, '' escape).
std::string FormatLiteral(const Value& v) {
    if (v.IsNull()) return "NULL";
    if (v.type() == ColumnType::kString) {
        std::string out = "'";
        for (char c : v.GetString()) {
            if (c == '\'') {
                out += "''";
            } else {
                out.push_back(c);
            }
        }
        out.push_back('\'');
        return out;
    }
    return v.ToString();
}

}  // namespace

std::string Expr::ToString() const {
    switch (kind_) {
        case Kind::kLiteral:
            return "Literal(" + FormatLiteral(literal_) + ")";
        case Kind::kColumn:
            return "Column(" + column_name_ + ")";
        case Kind::kCompare: {
            std::string left = left_ ? left_->ToString() : "?";
            std::string right = right_ ? right_->ToString() : "?";
            return std::string("Compare(") + CmpOpName(cmp_op_) + ", " + left + ", " + right +
                   ")";
        }
        case Kind::kLogic:
            if (logic_op_ == LogicOp::kNot) {
                std::string child = left_ ? left_->ToString() : "?";
                return "Not(" + child + ")";
            }
            {
                std::string left = left_ ? left_->ToString() : "?";
                std::string right = right_ ? right_->ToString() : "?";
                return std::string(LogicOpName(logic_op_)) + "(" + left + ", " + right + ")";
            }
    }
    return "Expr(?)";
}

lsmkv::Status Expr::Bind(const TableSchema& schema) {
    switch (kind_) {
        case Kind::kLiteral:
            return STATUS(OK);
        case Kind::kColumn: {
            const int idx = schema.ColumnIndex(column_name_);
            if (idx < 0) {
                return STATUS(InvalidArgument, "unknown column: " + column_name_);
            }
            column_index_ = idx;
            return STATUS(OK);
        }
        case Kind::kCompare:
            RELDB_RETURN_NOT_OK(left_->Bind(schema));
            RELDB_RETURN_NOT_OK(right_->Bind(schema));
            return STATUS(OK);
        case Kind::kLogic:
            RELDB_RETURN_NOT_OK(left_->Bind(schema));
            if (logic_op_ != LogicOp::kNot) {
                RELDB_RETURN_NOT_OK(right_->Bind(schema));
            }
            return STATUS(OK);
    }
    return STATUS(InvalidArgument, "unknown expr kind");
}

lsmkv::Status Expr::Eval(const Row& row, const TableSchema& schema, Value* out) const {
    if (out == nullptr) return STATUS(InvalidArgument, "null out");
    switch (kind_) {
        case Kind::kLiteral:
            *out = literal_;
            return STATUS(OK);
        case Kind::kColumn: {
            int idx = column_index_;
            if (idx < 0) {
                idx = schema.ColumnIndex(column_name_);
                if (idx < 0) {
                    return STATUS(InvalidArgument, "unknown column: " + column_name_);
                }
            }
            if (static_cast<std::size_t>(idx) >= row.size()) {
                return STATUS(InvalidArgument, "row width mismatch");
            }
            *out = row.at(static_cast<std::size_t>(idx));
            return STATUS(OK);
        }
        case Kind::kCompare:
            return EvalCompare(row, schema, out);
        case Kind::kLogic:
            return EvalLogic(row, schema, out);
    }
    return STATUS(InvalidArgument, "unknown expr kind");
}

lsmkv::Status Expr::EvalBool(const Row& row, const TableSchema& schema, bool* out) const {
    if (out == nullptr) return STATUS(InvalidArgument, "null out");
    Value v;
    RELDB_RETURN_NOT_OK(Eval(row, schema, &v));
    // WHERE: SQL unknown (NULL) filters out the row.
    if (v.IsNull()) {
        *out = false;
        return STATUS(OK);
    }
    // Non-boolean predicates are a query error (e.g. WHERE id with no compare).
    if (v.type() != ColumnType::kBool) {
        return STATUS(InvalidArgument, "predicate must be boolean");
    }
    *out = v.GetBool();
    return STATUS(OK);
}

lsmkv::Status Expr::CompareValues(CmpOp op, const Value& left, const Value& right, Value* out) {
    if (left.IsNull() || right.IsNull()) {
        *out = Value::Null();
        return STATUS(OK);
    }
    if (left.type() != right.type()) {
        return STATUS(InvalidArgument, "compare type mismatch");
    }
    int cmp = 0;
    switch (left.type()) {
        case ColumnType::kInt64:
            cmp = (left.GetInt64() < right.GetInt64())
                      ? -1
                      : (left.GetInt64() > right.GetInt64() ? 1 : 0);
            break;
        case ColumnType::kString:
            cmp = left.GetString().compare(right.GetString());
            break;
        case ColumnType::kBool: {
            const int lb = left.GetBool() ? 1 : 0;
            const int rb = right.GetBool() ? 1 : 0;
            cmp = lb - rb;
            break;
        }
        case ColumnType::kNull:
            *out = Value::Null();
            return STATUS(OK);
    }
    bool result = false;
    switch (op) {
        case CmpOp::kEq: result = (cmp == 0); break;
        case CmpOp::kNe: result = (cmp != 0); break;
        case CmpOp::kLt: result = (cmp < 0); break;
        case CmpOp::kLe: result = (cmp <= 0); break;
        case CmpOp::kGt: result = (cmp > 0); break;
        case CmpOp::kGe: result = (cmp >= 0); break;
    }
    *out = Value::Bool(result);
    return STATUS(OK);
}

lsmkv::Status Expr::EvalCompare(const Row& row, const TableSchema& schema, Value* out) const {
    Value left;
    Value right;
    RELDB_RETURN_NOT_OK(left_->Eval(row, schema, &left));
    RELDB_RETURN_NOT_OK(right_->Eval(row, schema, &right));
    return CompareValues(cmp_op_, left, right, out);
}

lsmkv::Status Expr::EvalLogic(const Row& row, const TableSchema& schema, Value* out) const {
    // Three-valued logic with Null; EvalBool will treat Null as false for WHERE.
    if (logic_op_ == LogicOp::kNot) {
        Value v;
        RELDB_RETURN_NOT_OK(left_->Eval(row, schema, &v));
        if (v.IsNull()) {
            *out = Value::Null();
            return STATUS(OK);
        }
        if (v.type() != ColumnType::kBool) {
            return STATUS(InvalidArgument, "NOT requires boolean");
        }
        *out = Value::Bool(!v.GetBool());
        return STATUS(OK);
    }

    Value left;
    RELDB_RETURN_NOT_OK(left_->Eval(row, schema, &left));
    Value right;
    RELDB_RETURN_NOT_OK(right_->Eval(row, schema, &right));

    // Coerce non-bool non-null to error; null stays null.
    auto as_tri = [](const Value& v, int* tri) -> lsmkv::Status {
        // tri: -1 null, 0 false, 1 true
        if (v.IsNull()) {
            *tri = -1;
            return STATUS(OK);
        }
        if (v.type() != ColumnType::kBool) {
            return STATUS(InvalidArgument, "logic requires boolean");
        }
        *tri = v.GetBool() ? 1 : 0;
        return STATUS(OK);
    };
    int lt = 0;
    int rt = 0;
    RELDB_RETURN_NOT_OK(as_tri(left, &lt));
    RELDB_RETURN_NOT_OK(as_tri(right, &rt));

    if (logic_op_ == LogicOp::kAnd) {
        if (lt == 0 || rt == 0) {
            *out = Value::Bool(false);
            return STATUS(OK);
        }
        if (lt < 0 || rt < 0) {
            *out = Value::Null();
            return STATUS(OK);
        }
        *out = Value::Bool(true);
        return STATUS(OK);
    }
    // OR
    if (lt == 1 || rt == 1) {
        *out = Value::Bool(true);
        return STATUS(OK);
    }
    if (lt < 0 || rt < 0) {
        *out = Value::Null();
        return STATUS(OK);
    }
    *out = Value::Bool(false);
    return STATUS(OK);
}

}  // namespace reldb
