#include "reldb/sql_ast.h"

#include <cctype>
#include <string>
#include <type_traits>
#include <utility>

namespace reldb {
namespace {

std::string FormatValue(const Value& v) {
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

std::string TypeName(ColumnType t) {
    switch (t) {
        case ColumnType::kInt64: return "Int64";
        case ColumnType::kString: return "String";
        case ColumnType::kBool: return "Bool";
        case ColumnType::kNull: return "Null";
    }
    return "Unknown";
}

std::string JoinComma(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += ", ";
        out += parts[i];
    }
    return out;
}

std::string ToStringBegin(const BeginStmt&) { return "Begin"; }
std::string ToStringCommit(const CommitStmt&) { return "Commit"; }
std::string ToStringAbort(const AbortStmt&) { return "Abort"; }

std::string ToStringCreateTable(const CreateTableStmt& s) {
    std::vector<std::string> cols;
    cols.reserve(s.columns.size());
    for (const auto& c : s.columns) {
        std::string one = c.name + " " + TypeName(c.type);
        if (c.primary_key) one += " PRIMARY KEY";
        cols.push_back(std::move(one));
    }
    return "CreateTable(" + s.table_name + ", [" + JoinComma(cols) + "])";
}

std::string ToStringDropTable(const DropTableStmt& s) {
    std::string out = "DropTable(";
    if (s.if_exists) out += "IF EXISTS, ";
    out += s.table_name + ")";
    return out;
}

std::string ToStringAlterTableAddColumn(const AlterTableAddColumnStmt& s) {
    return "AlterTableAddColumn(" + s.table_name + ", " + s.column.name + " " +
           TypeName(s.column.type) + " DEFAULT " + FormatValue(s.default_value) + ")";
}

std::string ToStringAlterTableDropColumn(const AlterTableDropColumnStmt& s) {
    return "AlterTableDropColumn(" + s.table_name + ", " + s.column_name + ")";
}

std::string ToStringInsert(const InsertStmt& s) {
    std::string out = "Insert(" + s.table_name;
    if (!s.column_names.empty()) {
        out += ", cols=[" + JoinComma(s.column_names) + "]";
    }
    std::vector<std::string> vals;
    vals.reserve(s.values.size());
    for (const auto& v : s.values) {
        vals.push_back(FormatValue(v));
    }
    out += ", values=[" + JoinComma(vals) + "])";
    return out;
}

std::string ToStringSelectItem(const SelectItem& item) {
    std::string out;
    if (item.kind == SelectItem::Kind::kAgg) {
        out = DefaultAggResultName(item.agg_func, item.agg_star, item.agg_column);
    } else {
        out = item.expr ? item.expr->ToString() : "?";
    }
    if (!item.alias.empty()) {
        out += " AS " + item.alias;
    }
    return out;
}

std::string ToStringSelect(const SelectStmt& s) {
    std::string out = "Select(";
    if (s.select_star) {
        out += "*";
    } else {
        std::vector<std::string> items;
        items.reserve(s.select_list.size());
        for (const auto& item : s.select_list) {
            items.push_back(ToStringSelectItem(item));
        }
        out += "[" + JoinComma(items) + "]";
    }
    out += " FROM ";
    out += s.from.base.table_name;
    if (!s.from.base.alias.empty()) {
        out += " AS " + s.from.base.alias;
    }
    for (const auto& j : s.from.joins) {
        out += " INNER JOIN " + j.right.table_name;
        if (!j.right.alias.empty()) {
            out += " AS " + j.right.alias;
        }
        out += " ON ";
        out += j.on ? j.on->ToString() : "?";
    }
    if (s.where) {
        out += " WHERE " + s.where->ToString();
    }
    if (!s.group_by.empty()) {
        out += " GROUP BY [" + JoinComma(s.group_by) + "]";
    }
    if (s.having) {
        out += " HAVING " + s.having->ToString();
    }
    if (!s.order_by.empty()) {
        std::vector<std::string> ord;
        ord.reserve(s.order_by.size());
        for (const auto& o : s.order_by) {
            ord.push_back(o.column_name + (o.ascending ? " ASC" : " DESC"));
        }
        out += " ORDER BY [" + JoinComma(ord) + "]";
    }
    if (s.has_limit) {
        out += " LIMIT " + std::to_string(s.limit);
    }
    out += ")";
    return out;
}

std::string ToStringUpdate(const UpdateStmt& s) {
    std::vector<std::string> assigns;
    assigns.reserve(s.sets.size());
    for (const auto& a : s.sets) {
        assigns.push_back(a.column_name + "=" + (a.value ? a.value->ToString() : "?"));
    }
    std::string out = "Update(" + s.table_name + ", SET [" + JoinComma(assigns) + "]";
    if (s.where) {
        out += " WHERE " + s.where->ToString();
    }
    out += ")";
    return out;
}

std::string ToStringDelete(const DeleteStmt& s) {
    std::string out = "Delete(" + s.table_name;
    if (s.where) {
        out += " WHERE " + s.where->ToString();
    }
    out += ")";
    return out;
}

}  // namespace

const char* AggFuncName(AggFunc f) {
    switch (f) {
        case AggFunc::kCount: return "COUNT";
        case AggFunc::kSum: return "SUM";
        case AggFunc::kAvg: return "AVG";
        case AggFunc::kMin: return "MIN";
        case AggFunc::kMax: return "MAX";
    }
    return "AGG";
}

std::string DefaultAggResultName(AggFunc f, bool star, const std::string& col) {
    std::string s = AggFuncName(f);
    s += '(';
    if (star) {
        s += '*';
    } else {
        s += col;
    }
    s += ')';
    return s;
}

bool ParseDefaultAggResultName(const std::string& name, AggFunc* f, bool* star,
                               std::string* col) {
    if (f == nullptr || star == nullptr || col == nullptr) return false;
    auto starts = [&](const char* pfx) {
        return name.compare(0, std::char_traits<char>::length(pfx), pfx) == 0;
    };
    AggFunc func = AggFunc::kCount;
    const char* pfx = nullptr;
    if (starts("COUNT(")) {
        func = AggFunc::kCount;
        pfx = "COUNT(";
    } else if (starts("SUM(")) {
        func = AggFunc::kSum;
        pfx = "SUM(";
    } else if (starts("AVG(")) {
        func = AggFunc::kAvg;
        pfx = "AVG(";
    } else if (starts("MIN(")) {
        func = AggFunc::kMin;
        pfx = "MIN(";
    } else if (starts("MAX(")) {
        func = AggFunc::kMax;
        pfx = "MAX(";
    } else {
        return false;
    }
    if (name.empty() || name.back() != ')') return false;
    const std::size_t pfx_len = std::char_traits<char>::length(pfx);
    if (name.size() < pfx_len + 1) return false;
    const std::string inner = name.substr(pfx_len, name.size() - pfx_len - 1);
    if (inner == "*") {
        if (func != AggFunc::kCount) return false;
        *f = func;
        *star = true;
        col->clear();
        return true;
    }
    if (inner.empty()) return false;
    // Column name: identifier or qualifier.ident (no spaces / ops).
    for (char c : inner) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) {
            return false;
        }
    }
    // Reject leading/trailing/double dots (same shape as BindContext::SplitColumnRef).
    if (inner.front() == '.' || inner.back() == '.' ||
        inner.find("..") != std::string::npos) {
        return false;
    }
    *f = func;
    *star = false;
    *col = inner;
    return true;
}

std::string ToString(const Statement& stmt) {
    return std::visit(
        [](const auto& node) -> std::string {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, BeginStmt>) {
                return ToStringBegin(node);
            } else if constexpr (std::is_same_v<T, CommitStmt>) {
                return ToStringCommit(node);
            } else if constexpr (std::is_same_v<T, AbortStmt>) {
                return ToStringAbort(node);
            } else if constexpr (std::is_same_v<T, CreateTableStmt>) {
                return ToStringCreateTable(node);
            } else if constexpr (std::is_same_v<T, DropTableStmt>) {
                return ToStringDropTable(node);
            } else if constexpr (std::is_same_v<T, AlterTableAddColumnStmt>) {
                return ToStringAlterTableAddColumn(node);
            } else if constexpr (std::is_same_v<T, AlterTableDropColumnStmt>) {
                return ToStringAlterTableDropColumn(node);
            } else if constexpr (std::is_same_v<T, InsertStmt>) {
                return ToStringInsert(node);
            } else if constexpr (std::is_same_v<T, SelectStmt>) {
                return ToStringSelect(node);
            } else if constexpr (std::is_same_v<T, UpdateStmt>) {
                return ToStringUpdate(node);
            } else if constexpr (std::is_same_v<T, DeleteStmt>) {
                return ToStringDelete(node);
            }
        },
        stmt);
}

}  // namespace reldb
