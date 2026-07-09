#include "reldb/types.h"

namespace reldb {

const char* ColumnTypeName(ColumnType t) {
    switch (t) {
        case ColumnType::kNull: return "Null";
        case ColumnType::kInt64: return "Int64";
        case ColumnType::kString: return "String";
        case ColumnType::kBool: return "Bool";
    }
    return "Unknown";
}

std::string Value::ToString() const {
    switch (type_) {
        case ColumnType::kNull: return "NULL";
        case ColumnType::kInt64: return std::to_string(int_val_);
        case ColumnType::kString: return str_val_;
        case ColumnType::kBool: return bool_val_ ? "true" : "false";
    }
    return "?";
}

}  // namespace reldb
