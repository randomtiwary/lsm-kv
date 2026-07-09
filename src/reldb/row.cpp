#include "reldb/row.h"

#include "lsmkv/encoding.h"

namespace reldb {
namespace {

// Per-cell: u8 type | type-specific payload
//   Null:   (no payload)
//   Int64:  fixed64 (two's complement bit pattern as u64)
//   String: length-prefixed bytes
//   Bool:   u8 0/1

void AppendFixed64Bits(std::string* dst, std::int64_t v) {
    std::uint64_t bits = static_cast<std::uint64_t>(v);
    lsmkv::PutFixed64(dst, bits);
}

bool ConsumeFixed64Bits(lsmkv::Slice* input, std::int64_t* v) {
    std::uint64_t bits = 0;
    if (!lsmkv::GetFixed64(input, &bits)) return false;
    *v = static_cast<std::int64_t>(bits);
    return true;
}

}  // namespace

void Row::AppendValue(std::string* dst, const Value& v) {
    dst->push_back(static_cast<char>(v.type()));
    switch (v.type()) {
        case ColumnType::kNull:
            break;
        case ColumnType::kInt64:
            AppendFixed64Bits(dst, v.GetInt64());
            break;
        case ColumnType::kString:
            lsmkv::PutLengthPrefixedSlice(dst, v.GetString());
            break;
        case ColumnType::kBool:
            dst->push_back(v.GetBool() ? '\x01' : '\x00');
            break;
    }
}

bool Row::ConsumeValue(lsmkv::Slice* input, Value* out) {
    if (input->empty()) return false;
    auto type = static_cast<ColumnType>(static_cast<std::uint8_t>(input->data()[0]));
    input->remove_prefix(1);
    switch (type) {
        case ColumnType::kNull:
            *out = Value::Null();
            return true;
        case ColumnType::kInt64: {
            std::int64_t v = 0;
            if (!ConsumeFixed64Bits(input, &v)) return false;
            *out = Value::Int64(v);
            return true;
        }
        case ColumnType::kString: {
            lsmkv::Slice s;
            if (!lsmkv::GetLengthPrefixedSlice(input, &s)) return false;
            *out = Value::String(s.ToString());
            return true;
        }
        case ColumnType::kBool: {
            if (input->empty()) return false;
            *out = Value::Bool(input->data()[0] != 0);
            input->remove_prefix(1);
            return true;
        }
    }
    return false;
}

std::string Row::EncodeValue(const Value& v) {
    std::string out;
    AppendValue(&out, v);
    return out;
}

lsmkv::Status Row::DecodeValue(const std::string& bytes, Value* out) {
    lsmkv::Slice input(bytes);
    if (!ConsumeValue(&input, out)) {
        return lsmkv::Status::Corruption("bad value encoding");
    }
    if (!input.empty()) {
        return lsmkv::Status::Corruption("value: trailing bytes");
    }
    return lsmkv::Status::OK();
}

// Row: varint32 n | repeated Value
std::string Row::Encode() const {
    std::string out;
    lsmkv::PutVarint32(&out, static_cast<std::uint32_t>(values_.size()));
    for (const auto& v : values_) {
        AppendValue(&out, v);
    }
    return out;
}

lsmkv::Status Row::Decode(const std::string& bytes, Row* out) {
    if (out == nullptr) {
        return lsmkv::Status::InvalidArgument("null out");
    }
    lsmkv::Slice input(bytes);
    std::uint32_t n = 0;
    if (!lsmkv::GetVarint32(&input, &n)) {
        return lsmkv::Status::Corruption("row: bad count");
    }
    std::vector<Value> vals;
    vals.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        Value v;
        if (!ConsumeValue(&input, &v)) {
            return lsmkv::Status::Corruption("row: bad cell");
        }
        vals.push_back(std::move(v));
    }
    if (!input.empty()) {
        return lsmkv::Status::Corruption("row: trailing bytes");
    }
    *out = Row(std::move(vals));
    return lsmkv::Status::OK();
}

lsmkv::Status Row::ValidateAgainst(const TableSchema& schema) const {
    auto st = schema.Validate();
    if (!st.ok()) return st;
    if (values_.size() != schema.num_columns()) {
        return lsmkv::Status::InvalidArgument("row column count mismatch");
    }
    for (std::size_t i = 0; i < values_.size(); ++i) {
        const auto& col = schema.columns()[i];
        const auto& cell = values_[i];
        if (cell.IsNull()) {
            return lsmkv::Status::InvalidArgument("null not allowed in column: " + col.name);
        }
        if (cell.type() != col.type) {
            return lsmkv::Status::InvalidArgument(
                "type mismatch for column " + col.name + ": expected " +
                ColumnTypeName(col.type) + ", got " + ColumnTypeName(cell.type()));
        }
    }
    return lsmkv::Status::OK();
}

lsmkv::Status Row::PrimaryKey(const TableSchema& schema, Value* out) const {
    auto st = ValidateAgainst(schema);
    if (!st.ok()) return st;
    int pk = schema.primary_key_index();
    if (pk < 0) {
        return lsmkv::Status::InvalidArgument("no primary key");
    }
    *out = values_[static_cast<std::size_t>(pk)];
    return lsmkv::Status::OK();
}

}  // namespace reldb
