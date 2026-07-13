#include "reldb/schema.h"

#include "lsmkv/debug.h"
#include "lsmkv/encoding.h"
#include "lsmkv/slice.h"
#include "reldb/macros.h"

namespace reldb {
namespace {

// On-disk schema format version. Bump when the body layout changes.
// v1: name | ncols | (col_name, type, pk, flags)*
constexpr std::uint8_t kSchemaFormatVersion = 1;

}  // namespace

int TableSchema::primary_key_index() const {
    // Callers should Validate() first (exactly one PK). Multiple PKs are a bug.
    int found = -1;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].primary_key) {
            LSMKV_DCHECK(found < 0);
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
        return STATUS(InvalidArgument, "table name is empty");
    }
    if (columns_.empty()) {
        return STATUS(InvalidArgument, "table has no columns");
    }
    int pk_count = 0;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const auto& c = columns_[i];
        if (c.name.empty()) {
            return STATUS(InvalidArgument, "column name is empty");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (columns_[j].name == c.name) {
                return STATUS(InvalidArgument, "duplicate column: " + c.name);
            }
        }
        if (c.type == ColumnType::kNull) {
            return STATUS(InvalidArgument, "column type cannot be Null: " + c.name);
        }
        if (c.primary_key) {
            ++pk_count;
        }
    }
    if (pk_count != 1) {
        return STATUS(InvalidArgument, "table must have exactly one primary key");
    }
    return STATUS(OK);
}

// Encoding (format v1):
//   u8 version (=1)
//   varint32 name_len | name
//   varint32 num_columns
//   repeated: col_name | u8 type | u8 pk | u8 flags
std::string TableSchema::Encode() const {
    std::string out;
    out.push_back(static_cast<char>(kSchemaFormatVersion));
    lsmkv::PutLengthPrefixedSlice(&out, name_);
    lsmkv::PutVarint32(&out, static_cast<std::uint32_t>(columns_.size()));
    for (const auto& c : columns_) {
        lsmkv::PutLengthPrefixedSlice(&out, c.name);
        out.push_back(static_cast<char>(c.type));
        out.push_back(c.primary_key ? '\x01' : '\x00');
        out.push_back('\x00');  // flags (bit0 reserved for nullable)
    }
    return out;
}

lsmkv::Status TableSchema::Decode(const std::string& bytes, TableSchema* out) {
    if (out == nullptr) {
        return STATUS(InvalidArgument, "null out");
    }
    if (bytes.empty()) {
        return STATUS(Corruption, "schema: empty");
    }
    lsmkv::Slice input(bytes);
    const auto version = static_cast<std::uint8_t>(input.data()[0]);
    input.remove_prefix(1);
    if (version != kSchemaFormatVersion) {
        return STATUS(Corruption, "schema: unsupported format version");
    }

    lsmkv::Slice name_slice;
    if (!lsmkv::GetLengthPrefixedSlice(&input, &name_slice)) {
        return STATUS(Corruption, "schema: bad name");
    }
    std::uint32_t n = 0;
    if (!lsmkv::GetVarint32(&input, &n)) {
        return STATUS(Corruption, "schema: bad column count");
    }
    std::vector<ColumnDef> cols;
    cols.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        lsmkv::Slice cname;
        if (!lsmkv::GetLengthPrefixedSlice(&input, &cname)) {
            return STATUS(Corruption, "schema: bad column name");
        }
        if (input.size() < 3) {
            return STATUS(Corruption, "schema: truncated column meta");
        }
        ColumnDef col;
        col.name = cname.ToString();
        col.type = static_cast<ColumnType>(static_cast<std::uint8_t>(input.data()[0]));
        col.primary_key = (input.data()[1] != 0);
        const auto flags = static_cast<std::uint8_t>(input.data()[2]);
        if (flags != 0) {
            return STATUS(Corruption, "schema: unknown column flags");
        }
        input.remove_prefix(3);
        cols.push_back(std::move(col));
    }
    if (!input.empty()) {
        return STATUS(Corruption, "schema: trailing bytes");
    }
    *out = TableSchema(name_slice.ToString(), std::move(cols));
    return STATUS(OK);
}

}  // namespace reldb
