#include <string>

#include "test_harness.h"

#include "lsmkv/encoding.h"
#include "reldb/schema.h"
#include "reldb/types.h"

namespace {

// Build legacy (pre-B1) schema bytes the way catalog used to write them.
std::string EncodeLegacy(const reldb::TableSchema& s) {
    std::string out;
    lsmkv::PutLengthPrefixedSlice(&out, s.name());
    lsmkv::PutVarint32(&out, static_cast<std::uint32_t>(s.num_columns()));
    for (const auto& c : s.columns()) {
        lsmkv::PutLengthPrefixedSlice(&out, c.name);
        out.push_back(static_cast<char>(c.type));
        out.push_back(c.primary_key ? '\x01' : '\x00');
    }
    return out;
}

}  // namespace

TEST(reldb_schema_validate_ok) {
    reldb::TableSchema s("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
        {"active", reldb::ColumnType::kBool, false},
    });
    expect(s.Validate().ok(), "valid schema");
    expect_eq(s.primary_key_index(), 0, "pk idx");
    expect(s.FindColumn("name") != nullptr, "find name");
    expect_eq(s.ColumnIndex("active"), 2, "active idx");
    expect_eq(s.ColumnIndex("nope"), -1, "missing");
}

TEST(reldb_schema_validate_errors) {
    expect(reldb::TableSchema("", {{"id", reldb::ColumnType::kInt64, true}})
               .Validate()
               .IsInvalidArgument(),
           "empty name");
    expect(reldb::TableSchema("t", {}).Validate().IsInvalidArgument(), "no cols");
    expect(reldb::TableSchema("t", {{"id", reldb::ColumnType::kInt64, false}})
               .Validate()
               .IsInvalidArgument(),
           "no pk");
    expect(reldb::TableSchema("t", {
                                        {"id", reldb::ColumnType::kInt64, true},
                                        {"id2", reldb::ColumnType::kInt64, true},
                                    })
               .Validate()
               .IsInvalidArgument(),
           "two pk");
    expect(reldb::TableSchema("t", {
                                        {"a", reldb::ColumnType::kInt64, true},
                                        {"a", reldb::ColumnType::kString, false},
                                    })
               .Validate()
               .IsInvalidArgument(),
           "dup name");
    expect(reldb::TableSchema("t", {{"x", reldb::ColumnType::kNull, true}})
               .Validate()
               .IsInvalidArgument(),
           "null type");
}

TEST(reldb_schema_codec_roundtrip_versioned) {
    reldb::TableSchema s("accounts", {
        {"uid", reldb::ColumnType::kString, true},
        {"balance", reldb::ColumnType::kInt64, false},
        {"closed", reldb::ColumnType::kBool, false},
    });
    const std::string bytes = s.Encode();
    // Versioned magic SCH\x01
    expect(bytes.size() >= 4, "has magic");
    expect(bytes[0] == 'S' && bytes[1] == 'C' && bytes[2] == 'H' && bytes[3] == '\x01',
           "magic");

    reldb::TableSchema out;
    expect(reldb::TableSchema::Decode(bytes, &out).ok(), "decode");
    expect_eq(out.name(), std::string("accounts"), "name");
    expect_eq(out.num_columns(), static_cast<std::size_t>(3), "ncols");
    expect_eq(out.columns()[0].name, std::string("uid"), "c0");
    expect(out.columns()[0].primary_key, "pk");
    expect(out.columns()[1].type == reldb::ColumnType::kInt64, "c1 type");
    expect(out.Validate().ok(), "still valid");
}

// Golden: legacy name length 1 encodes first byte 0x01 — must not be mistaken for version.
TEST(reldb_schema_decode_legacy_name_len_1) {
    reldb::TableSchema s("t", {{"id", reldb::ColumnType::kInt64, true}});
    const std::string legacy = EncodeLegacy(s);
    expect(!legacy.empty() && legacy[0] == '\x01', "legacy starts with 0x01");

    reldb::TableSchema out;
    expect(reldb::TableSchema::Decode(legacy, &out).ok(), "decode legacy");
    expect_eq(out.name(), std::string("t"), "name");
    expect_eq(out.num_columns(), static_cast<std::size_t>(1), "ncols");
    expect(out.columns()[0].primary_key, "pk");
}

TEST(reldb_schema_decode_legacy_name_len_127) {
    const std::string long_name(127, 'x');
    reldb::TableSchema s(long_name, {
        {"id", reldb::ColumnType::kInt64, true},
        {"n", reldb::ColumnType::kString, false},
    });
    const std::string legacy = EncodeLegacy(s);
    expect(!legacy.empty() && static_cast<unsigned char>(legacy[0]) == 127, "varint 127");

    reldb::TableSchema out;
    expect(reldb::TableSchema::Decode(legacy, &out).ok(), "decode");
    expect_eq(out.name(), long_name, "name");
    expect_eq(out.num_columns(), static_cast<std::size_t>(2), "ncols");
}

TEST(reldb_schema_decode_legacy_accounts) {
    reldb::TableSchema s("accounts", {
        {"uid", reldb::ColumnType::kString, true},
        {"balance", reldb::ColumnType::kInt64, false},
        {"closed", reldb::ColumnType::kBool, false},
    });
    reldb::TableSchema out;
    expect(reldb::TableSchema::Decode(EncodeLegacy(s), &out).ok(), "decode");
    expect_eq(out.name(), std::string("accounts"), "name");
    expect_eq(out.columns()[2].name, std::string("closed"), "c2");
}

TEST(reldb_schema_upgrade_legacy_reencode) {
    // Simulate opening an old DB: decode legacy, re-Encode writes versioned.
    reldb::TableSchema s("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
    });
    reldb::TableSchema loaded;
    expect(reldb::TableSchema::Decode(EncodeLegacy(s), &loaded).ok(), "load legacy");
    const std::string v = loaded.Encode();
    expect(v.size() >= 4 && v[0] == 'S' && v[3] == '\x01', "re-encode versioned");
    reldb::TableSchema again;
    expect(reldb::TableSchema::Decode(v, &again).ok(), "decode versioned");
    expect_eq(again.name(), std::string("users"), "name");
}

TEST(reldb_schema_codec_corruption) {
    reldb::TableSchema out;
    expect(reldb::TableSchema::Decode("", &out).IsCorruption(), "empty");
    expect(reldb::TableSchema::Decode("\x01x", &out).IsCorruption(), "truncated legacy");

    // Truncated versioned (magic only).
    expect(reldb::TableSchema::Decode("SCH\x01", &out).IsCorruption(), "magic only");
    // Magic + truncated body.
    expect(reldb::TableSchema::Decode(std::string("SCH\x01", 4) + "\x01", &out).IsCorruption(),
           "trunc versioned");
    // Unknown flags bit on versioned column.
    reldb::TableSchema s("t", {{"id", reldb::ColumnType::kInt64, true}});
    std::string bad = s.Encode();
    // Last byte of first column is flags (0); set bit0.
    expect(!bad.empty(), "nonempty");
    bad.back() = '\x01';
    expect(reldb::TableSchema::Decode(bad, &out).IsCorruption(), "unknown flags");
}
