#include "test_harness.h"
#include "reldb/types.h"

TEST(reldb_value_equality_and_tostring) {
    expect(reldb::Value::Null() == reldb::Value::Null(), "null eq");
    expect(&reldb::Value::Null() == &reldb::Value::Null(), "null same instance");
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
