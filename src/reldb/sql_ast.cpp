#include "reldb/sql_ast.h"

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

std::string ToStringSelect(const SelectStmt& s) {
    std::string out = "Select(";
    if (s.select_star) {
        out += "*";
    } else {
        std::vector<std::string> items;
        items.reserve(s.select_list.size());
        for (const auto& e : s.select_list) {
            items.push_back(e ? e->ToString() : "?");
        }
        out += "[" + JoinComma(items) + "]";
    }
    out += " FROM " + s.table_name;
    if (s.where) {
        out += " WHERE " + s.where->ToString();
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
