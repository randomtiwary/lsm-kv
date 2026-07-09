#include "test_harness.h"
#include "reldb/types.h"
#include "reldb/row.h"
#include "reldb/schema.h"

TEST(reldb_value_equality_and_tostring) {
    expect(reldb::Value::Null() == reldb::Value::Null(), "null eq");
    expect(reldb::Value::Int64(42) == reldb::Value::Int64(42), "i64 eq");
    expect(reldb::Value::Int64(42) != reldb::Value::Int64(7), "i64 ne");
    expect(reldb::Value::String("a") == reldb::Value::String("a"), "str eq");
    expect(reldb::Value::Bool(true) == reldb::Value::Bool(true), "bool eq");
    expect(reldb::Value::Int64(1) != reldb::Value::String("1"), "cross type");
    expect_eq(reldb::Value::Int64(-5).ToString(), std::string("-5"), "i64 str");
    expect_eq(reldb::Value::Bool(false).ToString(), std::string("false"), "bool str");
    expect_eq(std::string(reldb::ColumnTypeName(reldb::ColumnType::kString)),
              std::string("String"), "type name");
}

TEST(reldb_value_codec_roundtrip) {
    auto check = [](const reldb::Value& v, const char* label) {
        std::string enc = reldb::Row::EncodeValue(v);
        reldb::Value out;
        expect(reldb::Row::DecodeValue(enc, &out).ok(), std::string("decode ") + label);
        expect(out == v, std::string("eq ") + label);
    };
    check(reldb::Value::Null(), "null");
    check(reldb::Value::Int64(0), "zero");
    check(reldb::Value::Int64(-1), "neg1");
    check(reldb::Value::Int64(9223372036854775807LL), "max");
    check(reldb::Value::Int64(-9223372036854775807LL - 1), "min");
    check(reldb::Value::String(""), "empty str");
    check(reldb::Value::String("hello"), "hello");
    check(reldb::Value::String(std::string(1000, 'x')), "long str");
    check(reldb::Value::Bool(true), "true");
    check(reldb::Value::Bool(false), "false");
}

TEST(reldb_value_codec_corruption) {
    reldb::Value v;
    expect(reldb::Row::DecodeValue("", &v).IsCorruption(), "empty");
    expect(reldb::Row::DecodeValue(std::string("\x01"), &v).IsCorruption(), "truncated int");
    // Valid int + trailing garbage
    std::string enc = reldb::Row::EncodeValue(reldb::Value::Int64(1));
    enc.push_back('x');
    expect(reldb::Row::DecodeValue(enc, &v).IsCorruption(), "trailing");
}

TEST(reldb_row_codec_roundtrip) {
    reldb::Row row({reldb::Value::Int64(7), reldb::Value::String("x"), reldb::Value::Bool(true)});
    std::string enc = row.Encode();
    reldb::Row out;
    expect(reldb::Row::Decode(enc, &out).ok(), "decode row");
    expect(out == row, "row eq");
    expect_eq(out.size(), static_cast<std::size_t>(3), "size");
}

TEST(reldb_row_empty_roundtrip) {
    reldb::Row row;
    reldb::Row out;
    expect(reldb::Row::Decode(row.Encode(), &out).ok(), "empty ok");
    expect_eq(out.size(), static_cast<std::size_t>(0), "empty size");
}

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
    reldb::TableSchema out;
    expect(reldb::TableSchema::Decode(s.Encode(), &out).ok(), "decode");
    expect_eq(out.name(), std::string("accounts"), "name");
    expect_eq(out.num_columns(), static_cast<std::size_t>(3), "ncols");
    expect_eq(out.columns()[0].name, std::string("uid"), "c0");
    expect(out.columns()[0].primary_key, "pk");
    expect(out.columns()[1].type == reldb::ColumnType::kInt64, "c1 type");
    expect(out.Validate().ok(), "still valid");
}

TEST(reldb_schema_codec_corruption) {
    reldb::TableSchema out;
    expect(reldb::TableSchema::Decode("", &out).IsCorruption(), "empty");
    expect(reldb::TableSchema::Decode("\x01x", &out).IsCorruption(), "truncated");
}

TEST(reldb_row_validate_against_schema) {
    reldb::TableSchema s("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
    });
    reldb::Row good({reldb::Value::Int64(1), reldb::Value::String("ann")});
    expect(good.ValidateAgainst(s).ok(), "good row");
    reldb::Value pk;
    expect(good.PrimaryKey(s, &pk).ok(), "pk");
    expect(pk == reldb::Value::Int64(1), "pk val");

    reldb::Row bad_type({reldb::Value::String("1"), reldb::Value::String("ann")});
    expect(bad_type.ValidateAgainst(s).IsInvalidArgument(), "type");

    reldb::Row bad_count({reldb::Value::Int64(1)});
    expect(bad_count.ValidateAgainst(s).IsInvalidArgument(), "count");

    reldb::Row with_null({reldb::Value::Int64(1), reldb::Value::Null()});
    expect(with_null.ValidateAgainst(s).IsInvalidArgument(), "null cell");
}
