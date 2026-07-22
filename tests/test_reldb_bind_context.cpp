#include <string>

#include "test_harness.h"

#include "reldb/bind_context.h"
#include "reldb/expr.h"
#include "reldb/schema.h"
#include "reldb/types.h"

namespace {

reldb::TableSchema UsersSchema() {
    return reldb::TableSchema("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
    });
}

reldb::TableSchema OrdersSchema() {
    return reldb::TableSchema("orders", {
        {"id", reldb::ColumnType::kInt64, true},
        {"uid", reldb::ColumnType::kInt64, false},
        {"amount", reldb::ColumnType::kInt64, false},
    });
}

}  // namespace

TEST(reldb_bind_context_single_table_alias) {
    reldb::BindContext ctx;
    EXPECT_OK(ctx.AddTable("users", "u", UsersSchema()), "add");
    expect_eq(ctx.num_tables(), 1, "1 table");
    expect_eq(ctx.total_columns(), 2, "2 cols");

    reldb::BoundColumn bc;
    EXPECT_OK(ctx.Resolve("id", &bc), "bare id");
    expect_eq(bc.column_index, 0, "idx");
    expect_eq(bc.row_offset, 0, "off");
    expect_eq(bc.column_name, std::string("id"), "name");

    EXPECT_OK(ctx.Resolve("u.id", &bc), "alias.id");
    expect_eq(bc.column_index, 0, "u.id idx");
    expect_eq(bc.column_name, std::string("id"), "bare");

    EXPECT_OK(ctx.Resolve("users.name", &bc), "table.name");
    expect_eq(bc.column_index, 1, "name idx");
    expect_eq(bc.row_offset, 1, "name off");

    expect(ctx.Resolve("nope", &bc).IsInvalidArgument(), "missing");
    expect(ctx.Resolve("x.id", &bc).IsInvalidArgument(), "bad table");
    expect(ctx.Resolve("u.nope", &bc).IsInvalidArgument(), "bad col");

    auto star = ctx.StarOutputNames();
    expect_eq(static_cast<int>(star.size()), 2, "star n");
    expect_eq(star[0], std::string("id"), "star0");
    expect_eq(star[1], std::string("name"), "star1");
}

TEST(reldb_bind_context_multi_table) {
    reldb::BindContext ctx;
    EXPECT_OK(ctx.AddTable("users", "u", UsersSchema()), "users");
    EXPECT_OK(ctx.AddTable("orders", "o", OrdersSchema()), "orders");
    expect_eq(ctx.total_columns(), 5, "5 cols");
    expect_eq(ctx.tables()[1].row_offset, 2, "orders offset");

    reldb::BoundColumn bc;
    // Shared name "id" is ambiguous unqualified.
    expect(ctx.Resolve("id", &bc).IsInvalidArgument(), "ambiguous id");
    auto amb = ctx.Resolve("id", &bc);
    expect(amb.ToString().find("ambiguous") != std::string::npos, "amb msg");

    EXPECT_OK(ctx.Resolve("u.id", &bc), "u.id");
    expect_eq(bc.table_index, 0, "users");
    expect_eq(bc.row_offset, 0, "u.id off");

    EXPECT_OK(ctx.Resolve("o.id", &bc), "o.id");
    expect_eq(bc.table_index, 1, "orders");
    expect_eq(bc.row_offset, 2, "o.id off");

    EXPECT_OK(ctx.Resolve("amount", &bc), "unique amount");
    expect_eq(bc.table_index, 1, "orders amount");
    expect_eq(bc.column_name, std::string("amount"), "amt name");

    EXPECT_OK(ctx.Resolve("orders.uid", &bc), "table qual");
    expect_eq(bc.row_offset, 3, "uid off");

    auto star = ctx.StarOutputNames();
    expect_eq(static_cast<int>(star.size()), 5, "star");
    expect_eq(star[0], std::string("u.id"), "u.id label");
    expect_eq(star[2], std::string("o.id"), "o.id label");
    expect_eq(star[4], std::string("o.amount"), "o.amount");
}

TEST(reldb_bind_context_duplicate_alias) {
    reldb::BindContext ctx;
    EXPECT_OK(ctx.AddTable("users", "t", UsersSchema()), "u");
    expect(ctx.AddTable("orders", "t", OrdersSchema()).IsInvalidArgument(), "dup alias");
    expect(ctx.AddTable("users", "", UsersSchema()).IsInvalidArgument(), "dup table");
}

TEST(reldb_bind_context_expr_bind) {
    reldb::BindContext ctx;
    EXPECT_OK(ctx.AddTable("users", "u", UsersSchema()), "add");

    auto eq = reldb::Expr::Compare(
        reldb::CmpOp::kEq, reldb::Expr::Column("u.id"),
        reldb::Expr::Literal(reldb::Value::Int64(1)));
    EXPECT_OK(eq->Bind(ctx), "bind");
    // Rewritten to bare name for single-table PK matchers.
    expect_eq(eq->left()->column_name(), std::string("id"), "bare");
    expect_eq(eq->left()->column_index(), 0, "idx");
}

TEST(reldb_bind_context_split_ref) {
    std::string q, c;
    EXPECT_OK(reldb::BindContext::SplitColumnRef("id", &q, &c), "bare");
    expect(q.empty(), "no qual");
    expect_eq(c, std::string("id"), "col");
    EXPECT_OK(reldb::BindContext::SplitColumnRef("u.id", &q, &c), "qual");
    expect_eq(q, std::string("u"), "u");
    expect_eq(c, std::string("id"), "id");
    expect(reldb::BindContext::SplitColumnRef("a.b.c", &q, &c).IsInvalidArgument(), "multi");
    expect(reldb::BindContext::SplitColumnRef(".id", &q, &c).IsInvalidArgument(), "lead");
}
