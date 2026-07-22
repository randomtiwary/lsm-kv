#include "reldb/bind_context.h"

#include "reldb/macros.h"

namespace reldb {

lsmkv::Status BindContext::SplitColumnRef(const std::string& ref, std::string* qualifier,
                                          std::string* column) {
    if (qualifier == nullptr || column == nullptr) {
        return STATUS(InvalidArgument, "null out");
    }
    if (ref.empty()) {
        return STATUS(InvalidArgument, "empty column reference");
    }
    const auto dot = ref.find('.');
    if (dot == std::string::npos) {
        qualifier->clear();
        *column = ref;
        return STATUS(OK);
    }
    if (ref.find('.', dot + 1) != std::string::npos) {
        return STATUS(InvalidArgument, "invalid column reference: " + ref);
    }
    if (dot == 0 || dot + 1 >= ref.size()) {
        return STATUS(InvalidArgument, "invalid column reference: " + ref);
    }
    *qualifier = ref.substr(0, dot);
    *column = ref.substr(dot + 1);
    return STATUS(OK);
}

lsmkv::Status BindContext::AddTable(std::string table_name, std::string alias, TableSchema schema) {
    if (table_name.empty()) {
        return STATUS(InvalidArgument, "empty table name in FROM");
    }

    auto register_name = [&](const std::string& name) -> lsmkv::Status {
        if (name.empty()) return STATUS(OK);
        if (name_to_table_.find(name) != name_to_table_.end()) {
            return STATUS(InvalidArgument, "duplicate table name or alias: " + name);
        }
        name_to_table_[name] = static_cast<int>(tables_.size());
        return STATUS(OK);
    };

    RELDB_RETURN_NOT_OK(register_name(table_name));
    if (!alias.empty() && alias != table_name) {
        RELDB_RETURN_NOT_OK(register_name(alias));
    }

    BoundTable bt;
    bt.table_name = table_name;
    bt.alias = alias;
    bt.schema = std::move(schema);
    bt.row_offset = total_columns_;
    total_columns_ += static_cast<int>(bt.schema.num_columns());
    tables_.push_back(std::move(bt));
    return STATUS(OK);
}

int BindContext::FindTable(const std::string& qualifier) const {
    const auto it = name_to_table_.find(qualifier);
    if (it == name_to_table_.end()) return -1;
    return it->second;
}

lsmkv::Status BindContext::Resolve(const std::string& ref, BoundColumn* out) const {
    if (out == nullptr) return STATUS(InvalidArgument, "null out");
    std::string qual;
    std::string col;
    RELDB_RETURN_NOT_OK(SplitColumnRef(ref, &qual, &col));

    if (!qual.empty()) {
        const int ti = FindTable(qual);
        if (ti < 0) {
            return STATUS(InvalidArgument, "unknown table: " + qual);
        }
        const BoundTable& t = tables_[static_cast<std::size_t>(ti)];
        const int ci = t.schema.ColumnIndex(col);
        if (ci < 0) {
            return STATUS(InvalidArgument, "unknown column: " + ref);
        }
        out->table_index = ti;
        out->column_index = ci;
        out->row_offset = t.row_offset + ci;
        out->type = t.schema.columns()[static_cast<std::size_t>(ci)].type;
        out->column_name = col;
        return STATUS(OK);
    }

    // Unqualified: must match exactly one table.
    int found_ti = -1;
    int found_ci = -1;
    for (std::size_t ti = 0; ti < tables_.size(); ++ti) {
        const int ci = tables_[ti].schema.ColumnIndex(col);
        if (ci < 0) continue;
        if (found_ti >= 0) {
            return STATUS(InvalidArgument, "ambiguous column: " + col);
        }
        found_ti = static_cast<int>(ti);
        found_ci = ci;
    }
    if (found_ti < 0) {
        return STATUS(InvalidArgument, "unknown column: " + col);
    }
    const BoundTable& t = tables_[static_cast<std::size_t>(found_ti)];
    out->table_index = found_ti;
    out->column_index = found_ci;
    out->row_offset = t.row_offset + found_ci;
    out->type = t.schema.columns()[static_cast<std::size_t>(found_ci)].type;
    out->column_name = col;
    return STATUS(OK);
}

std::vector<std::string> BindContext::StarOutputNames() const {
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(total_columns_));
    const bool qualify = tables_.size() > 1;
    for (const auto& t : tables_) {
        const std::string& corr = t.CorrelationName();
        for (const auto& c : t.schema.columns()) {
            if (qualify) {
                names.push_back(corr + "." + c.name);
            } else {
                names.push_back(c.name);
            }
        }
    }
    return names;
}

}  // namespace reldb
