#include <memory>
#include <string>

#include "test_harness.h"

#include "reldb/expr.h"
#include "reldb/sql_ast.h"
#include "reldb/types.h"

TEST(reldb_sql_ast_txn_and_predicates) {
    reldb::Statement begin = reldb::BeginStmt{};
    reldb::Statement commit = reldb::CommitStmt{};
    reldb::Statement abort = reldb::AbortStmt{};
    expect(reldb::IsBegin(begin), "begin");
    expect(reldb::IsCommit(commit), "commit");
    expect(reldb::IsAbort(abort), "abort");
    expect_eq(reldb::ToString(begin), std::string("Begin"), "begin str");
    expect_eq(reldb::ToString(commit), std::string("Commit"), "commit str");
    expect_eq(reldb::ToString(abort), std::string("Abort"), "abort str");
}

TEST(reldb_sql_ast_create_table_print) {
    reldb::CreateTableStmt ct;
    ct.table_name = "users";
    ct.columns.push_back({"id", reldb::ColumnType::kInt64, true});
    ct.columns.push_back({"name", reldb::ColumnType::kString, false});
    ct.columns.push_back({"active", reldb::ColumnType::kBool, false});
    reldb::Statement s = std::move(ct);
    expect(reldb::IsCreateTable(s), "kind");
    expect_eq(reldb::ToString(s),
              std::string("CreateTable(users, [id Int64 PRIMARY KEY, name String, active Bool])"),
              "print");
}

TEST(reldb_sql_ast_drop_table_print) {
    reldb::DropTableStmt dt;
    dt.table_name = "users";
    reldb::Statement s = std::move(dt);
    expect(reldb::IsDropTable(s), "kind");
    expect_eq(reldb::ToString(s), std::string("DropTable(users)"), "print");

    reldb::DropTableStmt dt2;
    dt2.table_name = "users";
    dt2.if_exists = true;
    reldb::Statement s2 = std::move(dt2);
    expect_eq(reldb::ToString(s2), std::string("DropTable(IF EXISTS, users)"), "if exists");
}

TEST(reldb_sql_ast_alter_table_print) {
    reldb::AlterTableAddColumnStmt add;
    add.table_name = "users";
    add.column = {"age", reldb::ColumnType::kInt64, false};
    add.default_value = reldb::Value::Int64(0);
    reldb::Statement s = std::move(add);
    expect(reldb::IsAlterTableAddColumn(s), "add kind");
    expect_eq(reldb::ToString(s),
              std::string("AlterTableAddColumn(users, age Int64 DEFAULT 0)"), "add print");

    reldb::AlterTableDropColumnStmt drop;
    drop.table_name = "users";
    drop.column_name = "age";
    reldb::Statement s2 = std::move(drop);
    expect(reldb::IsAlterTableDropColumn(s2), "drop kind");
    expect_eq(reldb::ToString(s2), std::string("AlterTableDropColumn(users, age)"),
              "drop print");
}

TEST(reldb_sql_ast_insert_print) {
    reldb::InsertStmt ins;
    ins.table_name = "users";
    ins.column_names = {"id", "name"};
    ins.values = {reldb::Value::Int64(1), reldb::Value::String("ada")};
    reldb::Statement s = std::move(ins);
    expect_eq(reldb::ToString(s),
              std::string("Insert(users, cols=[id, name], values=[1, 'ada'])"), "print");

    reldb::InsertStmt ins2;
    ins2.table_name = "t";
    ins2.values = {reldb::Value::String("it's"), reldb::Value::Bool(true), reldb::Value::Null()};
    reldb::Statement s2 = std::move(ins2);
    expect_eq(reldb::ToString(s2),
              std::string("Insert(t, values=['it''s', true, NULL])"), "escape null");
}

TEST(reldb_sql_ast_select_print) {
    reldb::SelectStmt sel;
    sel.select_star = true;
    sel.from.base.table_name = "users";
    reldb::Statement s = std::move(sel);
    expect_eq(reldb::ToString(s), std::string("Select(* FROM users)"), "star");

    reldb::SelectStmt sel2;
    sel2.from.base.table_name = "users";
    sel2.select_list.push_back(reldb::MakeExprSelectItem(reldb::Expr::Column("name")));
    sel2.where = reldb::Expr::Compare(reldb::CmpOp::kEq, reldb::Expr::Column("id"),
                                      reldb::Expr::Literal(reldb::Value::Int64(1)));
    sel2.order_by.push_back({"name", false});
    sel2.has_limit = true;
    sel2.limit = 10;
    reldb::Statement s2 = std::move(sel2);
    expect_eq(reldb::ToString(s2),
              std::string("Select([Column(name)] FROM users WHERE "
                          "Compare(Eq, Column(id), Literal(1)) ORDER BY [name DESC] LIMIT 10)"),
              "full");
}

TEST(reldb_sql_ast_agg_result_name) {
    expect_eq(reldb::DefaultAggResultName(reldb::AggFunc::kCount, true, ""),
              std::string("COUNT(*)"), "count star");
    expect_eq(reldb::DefaultAggResultName(reldb::AggFunc::kSum, false, "score"),
              std::string("SUM(score)"), "sum");
    reldb::AggFunc f;
    bool star = false;
    std::string col;
    expect(reldb::ParseDefaultAggResultName("COUNT(*)", &f, &star, &col), "parse count");
    expect(f == reldb::AggFunc::kCount, "f count");
    expect(star, "star");
    expect(reldb::ParseDefaultAggResultName("AVG(score)", &f, &star, &col), "parse avg");
    expect(f == reldb::AggFunc::kAvg && !star && col == "score", "avg parts");
    expect(!reldb::ParseDefaultAggResultName("n", &f, &star, &col), "not agg");
}

// SelectStmt shape: FromClause, SelectItem, group_by / having fields.
TEST(reldb_sql_ast_select_stmt_shape) {
    reldb::SelectStmt sel;
    sel.from.base.table_name = "t";
    sel.select_list.push_back(reldb::MakeExprSelectItem(reldb::Expr::Column("id")));
    expect(sel.group_by.empty(), "group_by empty");
    expect(sel.having == nullptr, "having null");
    expect_eq(sel.from.base.table_name, std::string("t"), "from table");
    expect(sel.from.joins.empty(), "no joins");
    expect_eq(static_cast<int>(sel.select_list.size()), 1, "one item");
    expect(sel.select_list[0].kind == reldb::SelectItem::Kind::kExpr, "expr kind");

    // Aggregate SelectItem ToString (parser does not produce this yet).
    reldb::SelectItem agg;
    agg.kind = reldb::SelectItem::Kind::kAgg;
    agg.agg_func = reldb::AggFunc::kCount;
    agg.agg_star = true;
    sel.select_list.clear();
    sel.select_list.push_back(std::move(agg));
    sel.group_by = {"dept"};
    reldb::Statement s = std::move(sel);
    expect_eq(reldb::ToString(s),
              std::string("Select([COUNT(*)] FROM t GROUP BY [dept])"), "agg print");
}

TEST(reldb_sql_ast_update_delete_print) {
    reldb::UpdateStmt up;
    up.table_name = "users";
    up.sets.push_back({"name", reldb::Expr::Literal(reldb::Value::String("bob"))});
    up.where = reldb::Expr::Compare(reldb::CmpOp::kEq, reldb::Expr::Column("id"),
                                    reldb::Expr::Literal(reldb::Value::Int64(1)));
    reldb::Statement s = std::move(up);
    expect_eq(reldb::ToString(s),
              std::string("Update(users, SET [name=Literal('bob')] WHERE "
                          "Compare(Eq, Column(id), Literal(1)))"),
              "update");

    reldb::DeleteStmt del;
    del.table_name = "users";
    reldb::Statement s2 = std::move(del);
    expect_eq(reldb::ToString(s2), std::string("Delete(users)"), "delete all");

    reldb::DeleteStmt del2;
    del2.table_name = "users";
    del2.where = reldb::Expr::Column("active");
    reldb::Statement s3 = std::move(del2);
    expect_eq(reldb::ToString(s3), std::string("Delete(users WHERE Column(active))"), "del where");
}

TEST(reldb_sql_ast_expr_to_string) {
    auto e = reldb::Expr::And(
        reldb::Expr::Compare(reldb::CmpOp::kGt, reldb::Expr::Column("id"),
                             reldb::Expr::Literal(reldb::Value::Int64(0))),
        reldb::Expr::Not(reldb::Expr::Column("active")));
    expect_eq(e->ToString(),
              std::string("And(Compare(Gt, Column(id), Literal(0)), Not(Column(active)))"),
              "and not");

    auto or_e = reldb::Expr::Or(reldb::Expr::Literal(reldb::Value::Bool(true)),
                                reldb::Expr::Literal(reldb::Value::Null()));
    expect_eq(or_e->ToString(), std::string("Or(Literal(true), Literal(NULL))"), "or");
}
