#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "lsmkv/slice.h"
#include "lsmkv/status.h"
#include "reldb/schema.h"
#include "reldb/types.h"

namespace reldb {

// A row is an ordered list of Values matching a TableSchema's column order.
class Row {
public:
    Row() = default;
    explicit Row(std::vector<Value> values) : values_(std::move(values)) {}

    std::size_t size() const { return values_.size(); }
    const Value& at(std::size_t i) const { return values_.at(i); }
    Value& at(std::size_t i) { return values_.at(i); }
    const std::vector<Value>& values() const { return values_; }

    void Set(std::size_t i, Value v) { values_.at(i) = std::move(v); }
    void push_back(Value v) { values_.push_back(std::move(v)); }

    bool operator==(const Row& o) const { return values_ == o.values_; }
    bool operator!=(const Row& o) const { return !(*this == o); }

    // Check column count and that each value matches the column type.
    // v1: Null cells are not allowed in typed columns.
    lsmkv::Status ValidateAgainst(const TableSchema& schema) const;

    // Extract primary-key cell (requires a valid schema with one PK).
    lsmkv::Status PrimaryKey(const TableSchema& schema, Value* out) const;

    // Encode all cells; Decode reconstructs them.
    std::string Encode() const;
    static lsmkv::Status Decode(const std::string& bytes, Row* out);

    // Encode a single Value (used for PK materialization and cells).
    static std::string EncodeValue(const Value& v);
    static lsmkv::Status DecodeValue(const std::string& bytes, Value* out);
    static void AppendValue(std::string* dst, const Value& v);
    static bool ConsumeValue(lsmkv::Slice* input, Value* out);

private:
    std::vector<Value> values_;
};

}  // namespace reldb
