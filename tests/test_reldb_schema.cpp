#include <string>

#include "test_harness.h"

#include "reldb/schema.h"
#include "reldb/types.h"

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

TEST(reldb_schema_codec_roundtrip) {
    reldb::TableSchema s("accounts", {
        {"uid", reldb::ColumnType::kString, true},
        {"balance", reldb::ColumnType::kInt64, false},
        {"closed", reldb::ColumnType::kBool, false},
    });
    const std::string bytes = s.Encode();
    expect(!bytes.empty() && bytes[0] == '\x01', "format version 1");

    reldb::TableSchema out;
    expect(reldb::TableSchema::Decode(bytes, &out).ok(), "decode");
    expect_eq(out.name(), std::string("accounts"), "name");
    expect_eq(out.num_columns(), static_cast<std::size_t>(3), "ncols");
    expect_eq(out.columns()[0].name, std::string("uid"), "c0");
    expect(out.columns()[0].primary_key, "pk");
    expect(out.columns()[1].type == reldb::ColumnType::kInt64, "c1 type");
    expect(out.Validate().ok(), "still valid");
}

TEST(reldb_schema_codec_short_name) {
    // 1-char names are fine; no dual-read ambiguity (no legacy path).
    reldb::TableSchema s("t", {{"id", reldb::ColumnType::kInt64, true}});
    reldb::TableSchema out;
    expect(reldb::TableSchema::Decode(s.Encode(), &out).ok(), "decode");
    expect_eq(out.name(), std::string("t"), "name");
}

TEST(reldb_schema_codec_corruption) {
    reldb::TableSchema out;
    expect(reldb::TableSchema::Decode("", &out).IsCorruption(), "empty");
    expect(reldb::TableSchema::Decode("\x01", &out).IsCorruption(), "version only");
    expect(reldb::TableSchema::Decode("\x02\x01t\x01\x02id\x01\x01\x00", &out).IsCorruption(),
           "bad version");
    expect(reldb::TableSchema::Decode("\x01\x01", &out).IsCorruption(), "truncated body");

    reldb::TableSchema s("t", {{"id", reldb::ColumnType::kInt64, true}});
    std::string bad = s.Encode();
    bad.back() = '\x01';  // unknown flags
    expect(reldb::TableSchema::Decode(bad, &out).IsCorruption(), "unknown flags");
}
