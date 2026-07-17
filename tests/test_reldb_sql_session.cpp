#include <memory>
#include <string>

#include "test_harness.h"
#include "test_util.h"

#include "reldb/database.h"
#include "reldb/query_result.h"
#include "reldb/sql_session.h"

namespace {

std::shared_ptr<reldb::Database> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::shared_ptr<reldb::Database> db;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

}  // namespace

TEST(reldb_sql_session_e2e_basic) {
    auto dir = MakeTempDir("reldb_sql_sess_e2e");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        reldb::QueryResult r;

        EXPECT_OK(session.Execute(
                      "CREATE TABLE users(id INT PRIMARY KEY, name TEXT, score INT);", r),
                  "create");
        expect(!session.InTransaction(), "no txn");

        EXPECT_OK(session.Execute("BEGIN;", r), "begin");
        expect(session.InTransaction(), "in txn");
        EXPECT_OK(session.Execute("INSERT INTO users(id, name, score) VALUES (1, 'ada', 10);", r),
                  "ins1");
        expect_eq(r.rows_affected, static_cast<std::uint64_t>(1), "aff1");
        EXPECT_OK(session.Execute("INSERT INTO users(id, name, score) VALUES (2, 'bob', 30);", r),
                  "ins2");
        EXPECT_OK(session.Execute("INSERT INTO users(id, name, score) VALUES (3, 'cy', 20);", r),
                  "ins3");
        EXPECT_OK(session.Execute("SELECT * FROM users WHERE id = 1;", r), "sel point");
        expect_eq(static_cast<int>(r.rows.size()), 1, "one row");
        expect_eq(r.rows[0].at(1).GetString(), std::string("ada"), "ada");
        // Access path tag
        expect(r.plan_tag.find("PkPointGet") != std::string::npos, "point plan");
        EXPECT_OK(session.Execute("COMMIT;", r), "commit");
        expect(!session.InTransaction(), "out");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_sql_session_autocommit_and_filter) {
    auto dir = MakeTempDir("reldb_sql_sess_auto");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        reldb::QueryResult r;

        EXPECT_OK(session.Execute(
                      "CREATE TABLE t(id INT PRIMARY KEY, name TEXT, score INT);", r),
                  "create");
        // Autocommit inserts
        EXPECT_OK(session.Execute("INSERT INTO t VALUES (1, 'a', 5);", r), "i1");
        EXPECT_OK(session.Execute("INSERT INTO t VALUES (2, 'b', 15);", r), "i2");
        EXPECT_OK(session.Execute("INSERT INTO t VALUES (3, 'c', 25);", r), "i3");
        expect(!session.InTransaction(), "still no user txn");

        EXPECT_OK(session.Execute("SELECT name FROM t WHERE score > 10 ORDER BY name LIMIT 2;", r),
                  "sel");
        expect_eq(static_cast<int>(r.rows.size()), 2, "2 rows");
        expect_eq(r.column_names.size(), static_cast<std::size_t>(1), "1 col");
        // Filter residual → SeqScan in plan tag chain
        expect(r.plan_tag.find("SeqScan") != std::string::npos ||
                   r.plan_tag.find("Filter") != std::string::npos,
               "scan or filter");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_sql_session_update_delete) {
    auto dir = MakeTempDir("reldb_sql_sess_ud");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        reldb::QueryResult r;

        EXPECT_OK(session.Execute(
                      "CREATE TABLE t(id INT PRIMARY KEY, name TEXT);", r),
                  "create");
        EXPECT_OK(session.Execute("INSERT INTO t VALUES (1, 'ada');", r), "i1");
        EXPECT_OK(session.Execute("INSERT INTO t VALUES (2, 'bob');", r), "i2");

        EXPECT_OK(session.Execute("UPDATE t SET name = 'ada2' WHERE id = 1;", r), "upd");
        expect_eq(r.rows_affected, static_cast<std::uint64_t>(1), "1 upd");
        expect(r.plan_tag.find("Update") != std::string::npos, "upd tag");

        EXPECT_OK(session.Execute("SELECT name FROM t WHERE id = 1;", r), "chk");
        expect_eq(r.rows[0].at(0).GetString(), std::string("ada2"), "ada2");

        EXPECT_OK(session.Execute("DELETE FROM t WHERE id = 2;", r), "del");
        expect_eq(r.rows_affected, static_cast<std::uint64_t>(1), "1 del");

        EXPECT_OK(session.Execute("SELECT * FROM t;", r), "left");
        expect_eq(static_cast<int>(r.rows.size()), 1, "one left");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_sql_session_txn_errors_and_ddl_gate) {
    auto dir = MakeTempDir("reldb_sql_sess_gate");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        reldb::QueryResult r;

        EXPECT_OK(session.Execute("CREATE TABLE t(id INT PRIMARY KEY);", r), "create");

        // Double BEGIN
        EXPECT_OK(session.Execute("BEGIN;", r), "begin");
        expect(session.Execute("BEGIN;", r).IsInvalidArgument(), "double begin");

        // DDL inside txn rejected; catalog unchanged for new table
        expect(session.Execute("CREATE TABLE u(id INT PRIMARY KEY);", r).IsInvalidArgument(),
               "ddl in txn");
        expect(session.Execute("DROP TABLE t;", r).IsInvalidArgument(), "drop in txn");
        EXPECT_OK(session.Execute("ABORT;", r), "abort");

        // u should not exist — insert fails
        expect(!session.Execute("INSERT INTO u VALUES (1);", r).ok(), "no u");
        // t still exists (drop was blocked)
        EXPECT_OK(session.Execute("INSERT INTO t VALUES (1);", r), "t remains");

        // COMMIT without BEGIN
        expect(session.Execute("COMMIT;", r).IsInvalidArgument(), "commit none");
        expect(session.Execute("ABORT;", r).IsInvalidArgument(), "abort none");

        // DDL OK outside txn
        EXPECT_OK(session.Execute("CREATE TABLE u(id INT PRIMARY KEY);", r), "create u");
        EXPECT_OK(session.Execute("INSERT INTO u VALUES (1);", r), "ins u");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_sql_session_alter_table) {
    auto dir = MakeTempDir("reldb_sql_sess_alter");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        reldb::QueryResult r;

        EXPECT_OK(session.Execute(
                      "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);", r),
                  "create");
        EXPECT_OK(session.Execute("INSERT INTO users VALUES (1, 'a');", r), "ins");
        EXPECT_OK(session.Execute("INSERT INTO users VALUES (2, 'b');", r), "ins2");

        EXPECT_OK(session.Execute(
                      "ALTER TABLE users ADD COLUMN age INT DEFAULT 42;", r),
                  "add age");
        EXPECT_OK(session.Execute("SELECT * FROM users WHERE id = 1;", r), "sel");
        expect_eq(static_cast<int>(r.rows.size()), 1, "1 row");
        expect_eq(static_cast<int>(r.rows[0].size()), 3, "3 cells");
        expect_eq(r.rows[0].at(2).GetInt64(), static_cast<std::int64_t>(42), "default");

        EXPECT_OK(session.Execute("INSERT INTO users VALUES (3, 'c', 7);", r), "ins3");
        EXPECT_OK(session.Execute(
                      "ALTER TABLE users DROP COLUMN age;", r),
                  "drop age");
        EXPECT_OK(session.Execute("SELECT * FROM users WHERE id = 1;", r), "sel2");
        expect_eq(static_cast<int>(r.rows[0].size()), 2, "2 cells");
        expect_eq(r.rows[0].at(1).GetString(), std::string("a"), "name kept");

        // Cannot drop PK
        expect(session.Execute("ALTER TABLE users DROP COLUMN id;", r).IsInvalidArgument(),
               "drop pk");

        // DDL blocked in txn
        EXPECT_OK(session.Execute("BEGIN;", r), "begin");
        expect(session.Execute(
                          "ALTER TABLE users ADD COLUMN z INT DEFAULT 0;", r)
                   .IsInvalidArgument(),
               "add in txn");
        expect(session.Execute("ALTER TABLE users DROP COLUMN name;", r).IsInvalidArgument(),
               "drop in txn");
        EXPECT_OK(session.Execute("ABORT;", r), "abort");

        // name still present after blocked drop
        EXPECT_OK(session.Execute("SELECT name FROM users WHERE id = 1;", r), "name ok");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_sql_session_drop_table) {
    auto dir = MakeTempDir("reldb_sql_sess_drop");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        reldb::QueryResult r;

        EXPECT_OK(session.Execute(
                      "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);", r),
                  "create");
        EXPECT_OK(session.Execute("INSERT INTO users VALUES (1, 'a');", r), "ins");
        EXPECT_OK(session.Execute("INSERT INTO users VALUES (2, 'b');", r), "ins2");
        EXPECT_OK(session.Execute("SELECT * FROM users;", r), "sel");
        expect_eq(static_cast<int>(r.rows.size()), 2, "2 rows");

        EXPECT_OK(session.Execute("DROP TABLE users;", r), "drop");

        expect(session.Execute("SELECT * FROM users;", r).IsNotFound(), "gone");
        expect(session.Execute("INSERT INTO users VALUES (3, 'c');", r).IsNotFound(),
               "ins gone");
        // Plain DROP of missing table → NotFound
        expect(session.Execute("DROP TABLE users;", r).IsNotFound(), "drop missing");
        // IF EXISTS → success (no-op) when missing
        EXPECT_OK(session.Execute("DROP TABLE IF EXISTS users;", r), "if exists missing");
        EXPECT_OK(session.Execute("DROP TABLE IF EXISTS no_such_table;", r), "if exists never");

        // Recreate and use again
        EXPECT_OK(session.Execute(
                      "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);", r),
                  "recreate");
        EXPECT_OK(session.Execute("INSERT INTO users VALUES (9, 'z');", r), "ins3");
        // IF EXISTS on an existing table still drops it
        EXPECT_OK(session.Execute("DROP TABLE IF EXISTS users;", r), "if exists drop");
        expect(session.Execute("SELECT * FROM users;", r).IsNotFound(), "gone after if");

        EXPECT_OK(session.Execute(
                      "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);", r),
                  "recreate2");
        EXPECT_OK(session.Execute("INSERT INTO users VALUES (9, 'z');", r), "ins4");
        EXPECT_OK(session.Execute("SELECT * FROM users;", r), "sel2");
        expect_eq(static_cast<int>(r.rows.size()), 1, "1 row");
    }
    RemoveDirRecursive(dir);
}

// Aggregates and GROUP BY parse, but execution is not wired yet.
TEST(reldb_sql_session_aggregates_not_executed) {
    auto dir = MakeTempDir("reldb_sql_sess_agg");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        reldb::QueryResult r;

        EXPECT_OK(session.Execute(
                      "CREATE TABLE t(id INT PRIMARY KEY, score INT);", r),
                  "create");
        EXPECT_OK(session.Execute("INSERT INTO t VALUES (1, 10);", r), "ins");

        expect(session.Execute("SELECT COUNT(*) FROM t;", r).IsInvalidArgument(),
               "count not exec");
        expect(session.Execute("SELECT score FROM t GROUP BY score;", r).IsInvalidArgument(),
               "group not exec");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_sql_session_pk_point_plan_tag) {
    auto dir = MakeTempDir("reldb_sql_sess_plan");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        reldb::QueryResult r;

        EXPECT_OK(session.Execute("CREATE TABLE t(id INT PRIMARY KEY, v TEXT);", r), "c");
        EXPECT_OK(session.Execute("INSERT INTO t VALUES (7, 'x');", r), "i");
        EXPECT_OK(session.Execute("SELECT * FROM t WHERE id = 7;", r), "p");
        expect_eq(r.plan_tag, std::string("PkPointGet"), "tag");
        expect_eq(static_cast<int>(r.rows.size()), 1, "hit");

        EXPECT_OK(session.Execute("SELECT * FROM t WHERE id = 99;", r), "miss");
        expect_eq(static_cast<int>(r.rows.size()), 0, "empty");

        EXPECT_OK(session.Execute("SELECT * FROM t WHERE v = 'x';", r), "filter");
        expect(r.plan_tag.find("Filter") != std::string::npos ||
                   r.plan_tag == "Filter<-SeqScan",
               "filter plan");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_sql_session_read_your_writes) {
    auto dir = MakeTempDir("reldb_sql_sess_ryw");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        reldb::QueryResult r;

        EXPECT_OK(session.Execute("CREATE TABLE t(id INT PRIMARY KEY, name TEXT);", r), "c");
        EXPECT_OK(session.Execute("BEGIN;", r), "b");
        EXPECT_OK(session.Execute("INSERT INTO t VALUES (1, 'a');", r), "i");
        EXPECT_OK(session.Execute("SELECT * FROM t;", r), "s");
        expect_eq(static_cast<int>(r.rows.size()), 1, "sees insert");
        EXPECT_OK(session.Execute("COMMIT;", r), "cmt");
    }
    RemoveDirRecursive(dir);
}
