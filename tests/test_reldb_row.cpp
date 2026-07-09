#include "test_harness.h"
#include "reldb/row.h"
#include "reldb/schema.h"
#include "reldb/types.h"

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
