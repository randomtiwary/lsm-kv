#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace reldb {

// Logical column types supported in v1. Kept intentionally small for teaching.
enum class ColumnType : std::uint8_t {
    kNull = 0,
    kInt64 = 1,
    kString = 2,
    kBool = 3,
};

// Tagged union for a single cell. Educational: no move-only fancy storage.
class Value {
public:
    Value() : type_(ColumnType::kNull), int_val_(0), bool_val_(false) {}

    static Value Null() { return Value(); }
    static Value Int64(std::int64_t v) {
        Value x;
        x.type_ = ColumnType::kInt64;
        x.int_val_ = v;
        return x;
    }
    static Value String(std::string v) {
        Value x;
        x.type_ = ColumnType::kString;
        x.str_val_ = std::move(v);
        return x;
    }
    static Value Bool(bool v) {
        Value x;
        x.type_ = ColumnType::kBool;
        x.bool_val_ = v;
        return x;
    }

    ColumnType type() const { return type_; }
    bool IsNull() const { return type_ == ColumnType::kNull; }

    std::int64_t GetInt64() const { return int_val_; }
    const std::string& GetString() const { return str_val_; }
    bool GetBool() const { return bool_val_; }

    bool operator==(const Value& o) const {
        if (type_ != o.type_) return false;
        switch (type_) {
            case ColumnType::kNull: return true;
            case ColumnType::kInt64: return int_val_ == o.int_val_;
            case ColumnType::kString: return str_val_ == o.str_val_;
            case ColumnType::kBool: return bool_val_ == o.bool_val_;
        }
        return false;
    }
    bool operator!=(const Value& o) const { return !(*this == o); }

    // Human-readable form for tests and error messages.
    std::string ToString() const;

private:
    ColumnType type_;
    std::int64_t int_val_;
    std::string str_val_;
    bool bool_val_;
};

const char* ColumnTypeName(ColumnType t);

}  // namespace reldb
