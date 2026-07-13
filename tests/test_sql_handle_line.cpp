#include <memory>
#include <string>
#include <utility>

#include "test_harness.h"
#include "test_util.h"

#include "protocol.h"
#include "reldb/database.h"
#include "reldb/query_result.h"
#include "reldb/sql_session.h"
#include "sql_server.h"

namespace {

std::shared_ptr<reldb::Database> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::shared_ptr<reldb::Database> db;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

std::pair<bool, std::string> Feed(reldb::SqlSession& session, std::string& buf,
                                  const std::string& line) {
    std::string reply;
    const bool keep = reldb::SqlHandleLine(session, buf, line, &reply);
    return {keep, std::move(reply)};
}

}  // namespace

TEST(sql_handle_ping_quit_status) {
    auto dir = MakeTempDir("sql_h_meta");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        std::string buf;

        auto r = Feed(session, buf, "PING");
        expect(r.first, "keep after ping");
        expect_eq(r.second, std::string("+PONG\n"), "pong");

        r = Feed(session, buf, "  ping  ");  // case-insensitive + trim
        expect_eq(r.second, std::string("+PONG\n"), "pong lower");

        r = Feed(session, buf, "STATUS");
        expect_eq(r.second, std::string("+OK in_txn=0\n"), "status idle");

        r = Feed(session, buf, "QUIT");
        expect(!r.first, "close after quit");
        expect_eq(r.second, std::string("+OK\n"), "quit ok");
    }
    RemoveDirRecursive(dir);
}

TEST(sql_handle_execute_sql_and_select) {
    auto dir = MakeTempDir("sql_h_sql");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        std::string buf;

        auto r = Feed(session, buf, "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);");
        expect(r.first, "keep");
        expect_eq(r.second, std::string("+OK\n"), "create ok");

        r = Feed(session, buf, "INSERT INTO users VALUES (1, 'ada');");
        expect_eq(r.second, std::string("+OK rows_affected=1\n"), "insert");

        r = Feed(session, buf, "SELECT id, name FROM users WHERE id = 1;");
        expect(r.second.find("*RESULT 1 2\n") != std::string::npos, "result hdr");
        expect(r.second.find("$I:1\n") != std::string::npos, "id cell");
        expect(r.second.find("$S:ada\n") != std::string::npos, "name cell");
        expect(r.second.find("*END\n") != std::string::npos, "end");

        reldb::sqlproto::DecodedReply dec;
        EXPECT_OK(reldb::sqlproto::DecodeReply(r.second, &dec), "decode");
        expect(dec.kind == reldb::sqlproto::ReplyKind::kResult, "kind");
        expect_eq(dec.result.rows[0].at(1).GetString(), std::string("ada"), "ada");
    }
    RemoveDirRecursive(dir);
}

TEST(sql_handle_multiline_statement) {
    auto dir = MakeTempDir("sql_h_ml");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        std::string buf;

        reldb::QueryResult seed;
        EXPECT_OK(session.Execute(
                      "CREATE TABLE t(id INT PRIMARY KEY, name TEXT);"
                      "INSERT INTO t VALUES (1, 'x');",
                      seed),
                  "seed");

        auto r = Feed(session, buf, "SELECT id, name");
        expect(r.second.empty(), "no reply yet");
        expect(!buf.empty(), "buffer holds partial");

        r = Feed(session, buf, "FROM t");
        expect(r.second.empty(), "still partial");

        r = Feed(session, buf, "WHERE id = 1;");
        expect(!r.second.empty(), "complete reply");
        expect(buf.empty(), "buffer cleared");
        expect(r.second.find("$I:1\n") != std::string::npos, "id");
        expect(r.second.find("$S:x\n") != std::string::npos, "name");
    }
    RemoveDirRecursive(dir);
}

TEST(sql_handle_txn_stickiness) {
    auto dir = MakeTempDir("sql_h_txn");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        std::string buf;

        Feed(session, buf, "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);");

        auto r = Feed(session, buf, "BEGIN;");
        expect_eq(r.second, std::string("+OK\n"), "begin");
        expect(session.InTransaction(), "in txn");

        r = Feed(session, buf, "STATUS");
        expect_eq(r.second, std::string("+OK in_txn=1\n"), "status in txn");

        r = Feed(session, buf, "INSERT INTO users VALUES (1, 'ada');");
        expect_eq(r.second, std::string("+OK rows_affected=1\n"), "insert in txn");

        r = Feed(session, buf, "SELECT name FROM users WHERE id = 1;");
        expect(r.second.find("$S:ada\n") != std::string::npos, "sees uncommitted");

        r = Feed(session, buf, "COMMIT;");
        expect_eq(r.second, std::string("+OK\n"), "commit");
        expect(!session.InTransaction(), "not in txn");

        r = Feed(session, buf, "STATUS");
        expect_eq(r.second, std::string("+OK in_txn=0\n"), "status after commit");
    }
    RemoveDirRecursive(dir);
}

TEST(sql_handle_reset_on_empty_buffer) {
    auto dir = MakeTempDir("sql_h_reset2");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        std::string buf;

        Feed(session, buf, "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);");
        Feed(session, buf, "BEGIN;");
        Feed(session, buf, "INSERT INTO users VALUES (1, 'ghost');");
        expect(session.InTransaction(), "in txn");

        auto r = Feed(session, buf, "RESET");
        expect_eq(r.second, std::string("+OK\n"), "reset ok");
        expect(!session.InTransaction(), "txn aborted");
        expect(buf.empty(), "buf clear");

        // Ghost row must not be visible after abort.
        r = Feed(session, buf, "SELECT id FROM users WHERE id = 1;");
        // empty result set
        expect(r.second.find("*RESULT 0 ") != std::string::npos, "no row");
    }
    RemoveDirRecursive(dir);
}

TEST(sql_handle_ddl_error_in_txn) {
    auto dir = MakeTempDir("sql_h_ddlerr");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        std::string buf;

        Feed(session, buf, "BEGIN;");
        auto r = Feed(session, buf, "CREATE TABLE t(id INT PRIMARY KEY);");
        expect(r.second.find("-ERR InvalidArgument:") != std::string::npos, "err prefix");
        expect(r.second.find("DDL is not allowed inside a transaction") != std::string::npos,
               "err msg");
        expect(session.InTransaction(), "still in txn");
    }
    RemoveDirRecursive(dir);
}

TEST(sql_handle_statement_too_large) {
    auto dir = MakeTempDir("sql_h_big");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        std::string buf;

        // Fill near the cap then overflow.
        const std::string chunk(reldb::sqlproto::kMaxStatementBytes - 10, 'x');
        auto r = Feed(session, buf, chunk);
        expect(r.second.empty(), "partial no reply");
        expect(!buf.empty(), "buffered");

        r = Feed(session, buf, "yyyyyyyyyyyy");
        expect(r.second.find("statement too large") != std::string::npos, "too large");
        expect(buf.empty(), "cleared on overflow");
        expect(r.first, "keep open");

        // Connection still usable.
        r = Feed(session, buf, "STATUS");
        expect_eq(r.second, std::string("+OK in_txn=0\n"), "still works");
    }
    RemoveDirRecursive(dir);
}

TEST(sql_handle_meta_ignored_mid_statement) {
    auto dir = MakeTempDir("sql_h_meta_mid");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        reldb::SqlSession session(db);
        std::string buf;

        reldb::QueryResult seed;
        EXPECT_OK(session.Execute("CREATE TABLE t(id INT PRIMARY KEY);", seed), "seed");

        Feed(session, buf, "SELECT id");
        // "PING" mid-statement is just another line of SQL (will fail on execute).
        auto r = Feed(session, buf, "PING");
        expect(r.second.empty(), "no meta mid stream");
        expect(buf.find("PING") != std::string::npos, "appended");

        r = Feed(session, buf, "FROM t;");
        // Parse error expected
        expect(r.second.find("-ERR ") == 0, "sql error");
        expect(buf.empty(), "cleared after execute");
    }
    RemoveDirRecursive(dir);
}
