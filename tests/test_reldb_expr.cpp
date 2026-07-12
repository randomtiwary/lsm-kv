#include <memory>

#include "test_harness.h"

#include "reldb/expr.h"
#include "reldb/query_result.h"
#include "reldb/row.h"
#include "reldb/schema.h"
#include "reldb/types.h"

namespace {

reldb::TableSchema UsersSchema() {
    return reldb::TableSchema("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
        {"active", reldb::ColumnType::kBool, false},
    });
}

reldb::Row SampleRow() {
    return reldb::Row({
        reldb::Value::Int64(42),
        reldb::Value::String("ada"),
        reldb::Value::Bool(true),
    });
}

}  // namespace

TEST(reldb_expr_literal_and_column) {
    const auto schema = UsersSchema();
    const auto row = SampleRow();

    auto lit = reldb::Expr::Literal(reldb::Value::Int64(7));
    reldb::Value v;
    EXPECT_OK(lit->Eval(row, schema, &v), "lit");
    expect_eq(v.GetInt64(), static_cast<std::int64_t>(7), "7");

    auto col = reldb::Expr::Column("name");
    EXPECT_OK(col->Bind(schema), "bind");
    EXPECT_OK(col->Eval(row, schema, &v), "col");
    expect_eq(v.GetString(), std::string("ada"), "ada");

    auto missing = reldb::Expr::Column("nope");
    expect(missing->Bind(schema).IsInvalidArgument(), "bind missing");
}

TEST(reldb_expr_compare) {
    const auto schema = UsersSchema();
    const auto row = SampleRow();

    auto eq = reldb::Expr::Compare(
        reldb::CmpOp::kEq, reldb::Expr::Column("id"),
        reldb::Expr::Literal(reldb::Value::Int64(42)));
    EXPECT_OK(eq->Bind(schema), "bind");
    bool b = false;
    EXPECT_OK(eq->EvalBool(row, schema, &b), "eq");
    expect(b, "id=42");

    auto lt = reldb::Expr::Compare(
        reldb::CmpOp::kLt, reldb::Expr::Column("id"),
        reldb::Expr::Literal(reldb::Value::Int64(10)));
    EXPECT_OK(lt->Bind(schema), "bind lt");
    EXPECT_OK(lt->EvalBool(row, schema, &b), "lt");
    expect(!b, "not lt");

    auto ne = reldb::Expr::Compare(
        reldb::CmpOp::kNe, reldb::Expr::Column("name"),
        reldb::Expr::Literal(reldb::Value::String("bob")));
    EXPECT_OK(ne->Bind(schema), "bind ne");
    EXPECT_OK(ne->EvalBool(row, schema, &b), "ne");
    expect(b, "name != bob");

    auto mismatch = reldb::Expr::Compare(
        reldb::CmpOp::kEq, reldb::Expr::Column("id"),
        reldb::Expr::Literal(reldb::Value::String("x")));
    EXPECT_OK(mismatch->Bind(schema), "bind mm");
    expect(mismatch->EvalBool(row, schema, &b).IsInvalidArgument(), "type mismatch");
}

TEST(reldb_expr_logic_and_null) {
    const auto schema = UsersSchema();
    const auto row = SampleRow();

    auto active = reldb::Expr::Column("active");
    auto id_eq = reldb::Expr::Compare(
        reldb::CmpOp::kEq, reldb::Expr::Column("id"),
        reldb::Expr::Literal(reldb::Value::Int64(42)));
    auto both = reldb::Expr::And(std::move(active), std::move(id_eq));
    EXPECT_OK(both->Bind(schema), "bind and");
    bool b = false;
    EXPECT_OK(both->EvalBool(row, schema, &b), "and");
    expect(b, "true and true");

    auto not_active = reldb::Expr::Not(reldb::Expr::Column("active"));
    EXPECT_OK(not_active->Bind(schema), "bind not");
    EXPECT_OK(not_active->EvalBool(row, schema, &b), "not");
    expect(!b, "not true");

    // NULL compare → NULL → EvalBool false (filter out)
    auto null_cmp = reldb::Expr::Compare(
        reldb::CmpOp::kEq, reldb::Expr::Literal(reldb::Value::Null()),
        reldb::Expr::Literal(reldb::Value::Int64(1)));
    EXPECT_OK(null_cmp->Bind(schema), "bind null");
    reldb::Value v;
    EXPECT_OK(null_cmp->Eval(row, schema, &v), "eval null cmp");
    expect(v.IsNull(), "null result");
    EXPECT_OK(null_cmp->EvalBool(row, schema, &b), "bool null");
    expect(!b, "where null is false");

    // Bare non-bool column as predicate is a query error.
    auto bare_id = reldb::Expr::Column("id");
    EXPECT_OK(bare_id->Bind(schema), "bind id");
    expect(bare_id->EvalBool(row, schema, &b).IsInvalidArgument(), "non-bool predicate");

    // true OR NULL → true; false AND NULL → false
    auto or_null = reldb::Expr::Or(
        reldb::Expr::Literal(reldb::Value::Bool(true)),
        reldb::Expr::Compare(reldb::CmpOp::kEq, reldb::Expr::Literal(reldb::Value::Null()),
                             reldb::Expr::Literal(reldb::Value::Int64(1))));
    EXPECT_OK(or_null->Eval(row, schema, &v), "or null");
    expect(v.GetBool(), "true or null");

    auto and_null = reldb::Expr::And(
        reldb::Expr::Literal(reldb::Value::Bool(false)),
        reldb::Expr::Compare(reldb::CmpOp::kEq, reldb::Expr::Literal(reldb::Value::Null()),
                             reldb::Expr::Literal(reldb::Value::Int64(1))));
    EXPECT_OK(and_null->Eval(row, schema, &v), "and null");
    expect(!v.GetBool(), "false and null");
}

TEST(reldb_query_result_basic) {
    reldb::QueryResult r;
    expect(r.empty(), "empty");
    r.column_names = {"id", "name"};
    r.rows.push_back(SampleRow());
    r.rows_affected = 0;
    r.plan_tag = "SeqScan";
    expect(!r.empty(), "has rows");
    expect_eq(static_cast<int>(r.rows.size()), 1, "one row");
    expect_eq(r.plan_tag, std::string("SeqScan"), "tag");
    r.Clear();
    expect(r.empty(), "cleared");
    expect(r.plan_tag.empty(), "tag cleared");
}
