#include <string>
#include <vector>

#include "test_harness.h"

#include "reldb/expr.h"
#include "reldb/row.h"
#include "reldb/schema.h"
#include "reldb/sql_ast.h"
#include "reldb/sql_parser.h"
#include "reldb/types.h"

namespace {

reldb::TableSchema UsersSchema() {
    return reldb::TableSchema("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
        {"active", reldb::ColumnType::kBool, false},
    });
}

reldb::Row UserRow(std::int64_t id, const std::string& name, bool active) {
    return reldb::Row({reldb::Value::Int64(id), reldb::Value::String(name),
                       reldb::Value::Bool(active)});
}

}  // namespace

TEST(reldb_sql_parse_txn_control) {
    reldb::Statement s;
    EXPECT_OK(reldb::ParseStatement("BEGIN", &s), "begin");
    expect(reldb::IsBegin(s), "is begin");
    EXPECT_OK(reldb::ParseStatement("begin transaction;", &s), "begin txn");
    expect(reldb::IsBegin(s), "begin txn");

    EXPECT_OK(reldb::ParseStatement("COMMIT", &s), "commit");
    expect(reldb::IsCommit(s), "is commit");
    EXPECT_OK(reldb::ParseStatement("COMMIT TRANSACTION", &s), "commit txn");

    EXPECT_OK(reldb::ParseStatement("ABORT", &s), "abort");
    expect(reldb::IsAbort(s), "is abort");
    EXPECT_OK(reldb::ParseStatement("ROLLBACK", &s), "rollback");
    expect(reldb::IsAbort(s), "rollback is abort");
    EXPECT_OK(reldb::ParseStatement("rollback transaction;", &s), "rb txn");
}

TEST(reldb_sql_parse_create_table) {
    reldb::Statement s;
    EXPECT_OK(reldb::ParseStatement(
                  "CREATE TABLE users(id INT PRIMARY KEY, name TEXT, active BOOL)", &s),
              "create");
    expect(reldb::IsCreateTable(s), "kind");
    const auto& ct = std::get<reldb::CreateTableStmt>(s);
    expect_eq(ct.table_name, std::string("users"), "table");
    expect_eq(static_cast<int>(ct.columns.size()), 3, "3 cols");
    expect_eq(ct.columns[0].name, std::string("id"), "id");
    expect(ct.columns[0].type == reldb::ColumnType::kInt64, "int");
    expect(ct.columns[0].primary_key, "pk");
    expect_eq(ct.columns[1].name, std::string("name"), "name");
    expect(ct.columns[1].type == reldb::ColumnType::kString, "text");
    expect(!ct.columns[1].primary_key, "not pk");
    expect(ct.columns[2].type == reldb::ColumnType::kBool, "bool");
    expect_eq(reldb::ToString(s),
              std::string("CreateTable(users, [id Int64 PRIMARY KEY, name String, active Bool])"),
              "create print");

    // INTEGER / STRING / BOOLEAN synonyms
    EXPECT_OK(reldb::ParseStatement(
                  "CREATE TABLE t(a INTEGER PRIMARY KEY, b STRING, c BOOLEAN)", &s),
              "synonyms");

    // Missing PRIMARY KEY
    expect(reldb::ParseStatement("CREATE TABLE t(a INT, b TEXT)", &s).IsInvalidArgument(),
           "no pk");
    // Two PRIMARY KEYs
    expect(reldb::ParseStatement("CREATE TABLE t(a INT PRIMARY KEY, b INT PRIMARY KEY)", &s)
               .IsInvalidArgument(),
           "two pk");
}

TEST(reldb_sql_parse_drop_table) {
    reldb::Statement s;
    EXPECT_OK(reldb::ParseStatement("DROP TABLE users", &s), "drop");
    expect(reldb::IsDropTable(s), "kind");
    expect_eq(std::get<reldb::DropTableStmt>(s).table_name, std::string("users"), "name");
    expect(!std::get<reldb::DropTableStmt>(s).if_exists, "no if exists");
    expect_eq(reldb::ToString(s), std::string("DropTable(users)"), "print");

    EXPECT_OK(reldb::ParseStatement("DROP TABLE IF EXISTS users", &s), "if exists");
    expect(reldb::IsDropTable(s), "kind if");
    expect(std::get<reldb::DropTableStmt>(s).if_exists, "if exists flag");
    expect_eq(std::get<reldb::DropTableStmt>(s).table_name, std::string("users"), "name if");
    expect_eq(reldb::ToString(s), std::string("DropTable(IF EXISTS, users)"), "print if");

    // Missing table name
    expect(reldb::ParseStatement("DROP TABLE", &s).IsInvalidArgument(), "no name");
    expect(reldb::ParseStatement("DROP TABLE IF EXISTS", &s).IsInvalidArgument(), "if no name");
    // DROP without TABLE
    expect(reldb::ParseStatement("DROP users", &s).IsInvalidArgument(), "no TABLE");
    // IF without EXISTS
    expect(reldb::ParseStatement("DROP TABLE IF users", &s).IsInvalidArgument(), "if no exists");
}

TEST(reldb_sql_parse_alter_table) {
    reldb::Statement s;
    EXPECT_OK(reldb::ParseStatement(
                  "ALTER TABLE users ADD COLUMN age INT DEFAULT 0", &s),
              "add");
    expect(reldb::IsAlterTableAddColumn(s), "add kind");
    const auto& add = std::get<reldb::AlterTableAddColumnStmt>(s);
    expect_eq(add.table_name, std::string("users"), "table");
    expect_eq(add.column.name, std::string("age"), "col");
    expect(add.column.type == reldb::ColumnType::kInt64, "type");
    expect(!add.column.primary_key, "not pk");
    expect_eq(add.default_value.GetInt64(), static_cast<std::int64_t>(0), "def");
    expect_eq(reldb::ToString(s),
              std::string("AlterTableAddColumn(users, age Int64 DEFAULT 0)"), "print add");

    EXPECT_OK(reldb::ParseStatement(
                  "ALTER TABLE users ADD COLUMN city TEXT DEFAULT 'NYC'", &s),
              "add str");
    expect_eq(std::get<reldb::AlterTableAddColumnStmt>(s).default_value.GetString(),
              std::string("NYC"), "str def");

    EXPECT_OK(reldb::ParseStatement("ALTER TABLE users DROP COLUMN age", &s), "drop");
    expect(reldb::IsAlterTableDropColumn(s), "drop kind");
    expect_eq(std::get<reldb::AlterTableDropColumnStmt>(s).column_name, std::string("age"),
              "drop col");
    expect_eq(reldb::ToString(s), std::string("AlterTableDropColumn(users, age)"),
              "print drop");

    // Reject PRIMARY KEY on ADD
    expect(reldb::ParseStatement(
               "ALTER TABLE users ADD COLUMN x INT PRIMARY KEY DEFAULT 1", &s)
               .IsInvalidArgument(),
           "pk add");
    // Missing DEFAULT
    expect(reldb::ParseStatement("ALTER TABLE users ADD COLUMN x INT", &s)
               .IsInvalidArgument(),
           "no default");
    // Bad action
    expect(reldb::ParseStatement("ALTER TABLE users RENAME COLUMN a TO b", &s)
               .IsInvalidArgument(),
           "rename");
}

TEST(reldb_sql_parse_insert) {
    reldb::Statement s;
    EXPECT_OK(reldb::ParseStatement(
                  "INSERT INTO users(id, name) VALUES (1, 'ada')", &s),
              "insert");
    expect(reldb::IsInsert(s), "kind");
    const auto& ins = std::get<reldb::InsertStmt>(s);
    expect_eq(ins.table_name, std::string("users"), "table");
    expect_eq(static_cast<int>(ins.column_names.size()), 2, "2 cols");
    expect_eq(ins.column_names[0], std::string("id"), "id col");
    expect_eq(static_cast<int>(ins.values.size()), 2, "2 vals");
    expect_eq(ins.values[0].GetInt64(), static_cast<std::int64_t>(1), "1");
    expect_eq(ins.values[1].GetString(), std::string("ada"), "ada");

    // Escaped quote
    EXPECT_OK(reldb::ParseStatement("INSERT INTO t VALUES ('it''s')", &s), "escape");
    const auto& ins2 = std::get<reldb::InsertStmt>(s);
    expect_eq(ins2.values[0].GetString(), std::string("it's"), "escaped");
    expect(ins2.column_names.empty(), "no col list");

    // Bool / null
    EXPECT_OK(reldb::ParseStatement("INSERT INTO t VALUES (TRUE, FALSE, NULL)", &s),
              "bool null");
    const auto& ins3 = std::get<reldb::InsertStmt>(s);
    expect(ins3.values[0].GetBool(), "true");
    expect(!ins3.values[1].GetBool(), "false");
    expect(ins3.values[2].IsNull(), "null");

    // Column count mismatch
    expect(reldb::ParseStatement("INSERT INTO t(a, b) VALUES (1)", &s).IsInvalidArgument(),
           "mismatch");
    // Multi-row
    expect(reldb::ParseStatement("INSERT INTO t VALUES (1), (2)", &s).IsInvalidArgument(),
           "multirow");
}

TEST(reldb_sql_parse_select) {
    reldb::Statement s;
    EXPECT_OK(reldb::ParseStatement("SELECT * FROM users", &s), "star");
    expect(reldb::IsSelect(s), "kind");
    {
        const auto& sel = std::get<reldb::SelectStmt>(s);
        expect(sel.select_star, "star");
        expect_eq(sel.table_name, std::string("users"), "table");
        expect(sel.where == nullptr, "no where");
        expect(!sel.has_limit, "no limit");
    }

    EXPECT_OK(reldb::ParseStatement(
                  "SELECT name FROM users WHERE id = 1 ORDER BY name DESC LIMIT 10", &s),
              "full");
    {
        auto& sel = std::get<reldb::SelectStmt>(s);
        expect(!sel.select_star, "not star");
        expect_eq(static_cast<int>(sel.select_list.size()), 1, "1 proj");
        expect(sel.where != nullptr, "where");
        expect_eq(static_cast<int>(sel.order_by.size()), 1, "order");
        expect_eq(sel.order_by[0].column_name, std::string("name"), "order col");
        expect(!sel.order_by[0].ascending, "desc");
        expect(sel.has_limit, "limit");
        expect_eq(sel.limit, static_cast<std::int64_t>(10), "10");
        expect_eq(reldb::ToString(s),
                  std::string("Select([Column(name)] FROM users WHERE "
                              "Compare(Eq, Column(id), Literal(1)) ORDER BY [name DESC] LIMIT 10)"),
                  "select print");

        // Bind + eval WHERE id = 1
        auto schema = UsersSchema();
        EXPECT_OK(sel.where->Bind(schema), "bind");
        bool b = false;
        EXPECT_OK(sel.where->EvalBool(UserRow(1, "ada", true), schema, &b), "eval");
        expect(b, "id=1 true");
        EXPECT_OK(sel.where->EvalBool(UserRow(2, "bob", true), schema, &b), "eval2");
        expect(!b, "id=2 false");
    }

    // Complex WHERE
    EXPECT_OK(reldb::ParseStatement(
                  "SELECT * FROM users WHERE id > 0 AND NOT active OR name = 'x'", &s),
              "logic");
    {
        auto& sel = std::get<reldb::SelectStmt>(s);
        auto schema = UsersSchema();
        EXPECT_OK(sel.where->Bind(schema), "bind logic");
        bool b = false;
        // id=1>0, active=true → NOT active false; (true AND false) OR false = false
        EXPECT_OK(sel.where->EvalBool(UserRow(1, "ada", true), schema, &b), "e");
        expect(!b, "false");
        // active=false → NOT active true; true AND true = true
        EXPECT_OK(sel.where->EvalBool(UserRow(1, "ada", false), schema, &b), "e2");
        expect(b, "true");
    }
}

TEST(reldb_sql_parse_update_delete) {
    reldb::Statement s;
    EXPECT_OK(reldb::ParseStatement("UPDATE users SET name = 'bob', active = FALSE WHERE id = 1",
                                    &s),
              "update");
    expect(reldb::IsUpdate(s), "kind upd");
    {
        auto& u = std::get<reldb::UpdateStmt>(s);
        expect_eq(u.table_name, std::string("users"), "table");
        expect_eq(static_cast<int>(u.sets.size()), 2, "2 sets");
        expect_eq(u.sets[0].column_name, std::string("name"), "name");
        expect(u.where != nullptr, "where");
        auto schema = UsersSchema();
        EXPECT_OK(u.sets[0].value->Bind(schema), "bind set");
        reldb::Value v;
        EXPECT_OK(u.sets[0].value->Eval(UserRow(1, "x", true), schema, &v), "eval set");
        expect_eq(v.GetString(), std::string("bob"), "bob");
    }

    EXPECT_OK(reldb::ParseStatement("UPDATE users SET name = 'z'", &s), "upd no where");
    expect(std::get<reldb::UpdateStmt>(s).where == nullptr, "no where");

    EXPECT_OK(reldb::ParseStatement("DELETE FROM users WHERE id = 1", &s), "delete");
    expect(reldb::IsDelete(s), "kind del");
    expect(std::get<reldb::DeleteStmt>(s).where != nullptr, "del where");

    EXPECT_OK(reldb::ParseStatement("DELETE FROM users", &s), "del all");
    expect(std::get<reldb::DeleteStmt>(s).where == nullptr, "del no where");
}

TEST(reldb_sql_parse_script) {
    std::vector<reldb::Statement> stmts;
    EXPECT_OK(reldb::ParseScript(
                  "BEGIN;\n"
                  "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);\n"
                  "INSERT INTO users(id, name) VALUES (1, 'ada');\n"
                  "SELECT * FROM users WHERE id = 1;\n"
                  "COMMIT;\n",
                  &stmts),
              "script");
    expect_eq(static_cast<int>(stmts.size()), 5, "5 stmts");
    expect(reldb::IsBegin(stmts[0]), "0");
    expect(reldb::IsCreateTable(stmts[1]), "1");
    expect(reldb::IsInsert(stmts[2]), "2");
    expect(reldb::IsSelect(stmts[3]), "3");
    expect(reldb::IsCommit(stmts[4]), "4");

    // Empty / whitespace
    EXPECT_OK(reldb::ParseScript("   \n  ", &stmts), "empty");
    expect(stmts.empty(), "empty vec");

    // Extra semicolons
    EXPECT_OK(reldb::ParseScript("BEGIN;;;COMMIT;", &stmts), "extra semi");
    expect_eq(static_cast<int>(stmts.size()), 2, "2");
}

TEST(reldb_sql_parse_reject_unsupported) {
    reldb::Statement s;
    expect(reldb::ParseStatement("SELECT * FROM a JOIN b ON a.id = b.id", &s)
               .IsInvalidArgument(),
           "join");
    expect(reldb::ParseStatement("SELECT * FROM a, b", &s).IsInvalidArgument(), "comma join");
    expect(reldb::ParseStatement("SELECT * FROM t GROUP BY id", &s).IsInvalidArgument(),
           "group");
    expect(reldb::ParseStatement("SELECT DISTINCT name FROM t", &s).IsInvalidArgument(),
           "distinct");
    expect(reldb::ParseStatement("SELECT * FROM t UNION SELECT * FROM u", &s)
               .IsInvalidArgument(),
           "union");
    expect(reldb::ParseStatement("SELECT * FROM (SELECT * FROM t)", &s).IsInvalidArgument(),
           "subquery");
    expect(reldb::ParseStatement("SELECT name AS n FROM t", &s).IsInvalidArgument(), "as");

    // Trailing junk
    expect(reldb::ParseStatement("BEGIN COMMIT", &s).IsInvalidArgument(), "junk");
    // Bad syntax
    expect(reldb::ParseStatement("SELECT FROM t", &s).IsInvalidArgument(), "bad select");
    expect(reldb::ParseStatement("INSERT INTO t VALUES (", &s).IsInvalidArgument(), "unclosed");
    expect(reldb::ParseStatement("INSERT INTO t VALUES ('unterminated)", &s)
               .IsInvalidArgument(),
           "bad string");
}
