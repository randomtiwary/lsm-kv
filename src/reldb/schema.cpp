#include "reldb/schema.h"

#include "lsmkv/encoding.h"
#include "lsmkv/slice.h"

namespace reldb {

int TableSchema::primary_key_index() const {
    int found = -1;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].primary_key) {
            if (found >= 0) return -1;  // more than one
            found = static_cast<int>(i);
        }
    }
    return found;
}

const ColumnDef* TableSchema::FindColumn(const std::string& name) const {
    for (const auto& c : columns_) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

int TableSchema::ColumnIndex(const std::string& name) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

lsmkv::Status TableSchema::Validate() const {
    if (name_.empty()) {
        return lsmkv::Status::InvalidArgument("table name is empty");
    }
    if (columns_.empty()) {
        return lsmkv::Status::InvalidArgument("table has no columns");
    }
    int pk_count = 0;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const auto& c = columns_[i];
        if (c.name.empty()) {
            return lsmkv::Status::InvalidArgument("column name is empty");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (columns_[j].name == c.name) {
                return lsmkv::Status::InvalidArgument("duplicate column: " + c.name);
            }
        }
        if (c.type == ColumnType::kNull) {
            return lsmkv::Status::InvalidArgument("column type cannot be Null: " + c.name);
        }
        if (c.primary_key) {
            ++pk_count;
        }
    }
    if (pk_count != 1) {
        return lsmkv::Status::InvalidArgument("table must have exactly one primary key");
    }
    return lsmkv::Status::OK();
}

// Encoding:
//   varint32 name_len | name bytes
//   varint32 num_columns
//   repeated: varint32 col_name_len | col_name | u8 type | u8 primary_key
std::string TableSchema::Encode() const {
    std::string out;
    lsmkv::PutLengthPrefixedSlice(&out, name_);
    lsmkv::PutVarint32(&out, static_cast<std::uint32_t>(columns_.size()));
    for (const auto& c : columns_) {
        lsmkv::PutLengthPrefixedSlice(&out, c.name);
        out.push_back(static_cast<char>(c.type));
        out.push_back(c.primary_key ? '\x01' : '\x00');
    }
    return out;
}

lsmkv::Status TableSchema::Decode(const std::string& bytes, TableSchema* out) {
    if (out == nullptr) {
        return lsmkv::Status::InvalidArgument("null out");
    }
    lsmkv::Slice input(bytes);
    lsmkv::Slice name_slice;
    if (!lsmkv::GetLengthPrefixedSlice(&input, &name_slice)) {
        return lsmkv::Status::Corruption("schema: bad name");
    }
    std::uint32_t n = 0;
    if (!lsmkv::GetVarint32(&input, &n)) {
        return lsmkv::Status::Corruption("schema: bad column count");
    }
    std::vector<ColumnDef> cols;
    cols.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        lsmkv::Slice cname;
        if (!lsmkv::GetLengthPrefixedSlice(&input, &cname)) {
            return lsmkv::Status::Corruption("schema: bad column name");
        }
        if (input.size() < 2) {
            return lsmkv::Status::Corruption("schema: truncated column meta");
        }
        ColumnDef col;
        col.name = cname.ToString();
        col.type = static_cast<ColumnType>(static_cast<std::uint8_t>(input.data()[0]));
        col.primary_key = (input.data()[1] != 0);
        input.remove_prefix(2);
        cols.push_back(std::move(col));
    }
    if (!input.empty()) {
        return lsmkv::Status::Corruption("schema: trailing bytes");
    }
    *out = TableSchema(name_slice.ToString(), std::move(cols));
    return lsmkv::Status::OK();
}

}  // namespace reldb
