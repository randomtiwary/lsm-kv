// Minimal walk-through of the reldb SQL frontend (SqlSession).
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "lsmkv/options.h"
#include "lsmkv/status.h"
#include "reldb/database.h"
#include "reldb/query_result.h"
#include "reldb/sql_session.h"
#include "reldb/types.h"

namespace {

std::string MakeTempDir() {
    std::string path = "/tmp/reldb_sql_example_XXXXXX";
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');
    char* result = mkdtemp(buf.data());
    if (result == nullptr) return "/tmp/reldb_sql_example_fallback";
    return std::string(result);
}

void PrintResult(const std::string& label, const reldb::QueryResult& r) {
    std::cout << "== " << label;
    if (!r.plan_tag.empty()) {
        std::cout << "  [plan: " << r.plan_tag << "]";
    }
    std::cout << "\n";
    if (r.rows_affected != 0) {
        std::cout << "  rows_affected=" << r.rows_affected << "\n";
    }
    if (!r.column_names.empty()) {
        std::cout << "  cols:";
        for (const auto& c : r.column_names) {
            std::cout << " " << c;
        }
        std::cout << "\n";
    }
    for (const auto& row : r.rows) {
        std::cout << "  row:";
        for (std::size_t i = 0; i < row.size(); ++i) {
            std::cout << " " << row.at(i).ToString();
        }
        std::cout << "\n";
    }
    if (r.rows.empty() && r.rows_affected == 0) {
        std::cout << "  (empty)\n";
    }
}

bool Run(reldb::SqlSession& session, const char* sql, const char* label) {
    reldb::QueryResult result;
    const auto st = session.Execute(sql, result);
    if (!st.ok()) {
        std::cerr << label << " FAILED: " << st.ToString() << "\n  sql: " << sql << "\n";
        return false;
    }
    PrintResult(label, result);
    return true;
}

}  // namespace

int main() {
    const std::string path = MakeTempDir();
    std::cout << "db path: " << path << "\n";

    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::shared_ptr<reldb::Database> db;
    auto st = reldb::Database::Open(opt, path, &db);
    if (!st.ok()) {
        std::cerr << "open: " << st.ToString() << "\n";
        return 1;
    }

    reldb::SqlSession session(db);
    reldb::QueryResult result;

    // DDL is non-transactional and must run outside BEGIN…COMMIT.
    if (!Run(session,
             "CREATE TABLE users(id INT PRIMARY KEY, name TEXT, score INT);",
             "CREATE TABLE")) {
        return 1;
    }

    // Multi-statement transaction (snapshot isolation).
    if (!Run(session, "BEGIN;", "BEGIN")) return 1;
    if (!Run(session, "INSERT INTO users(id, name, score) VALUES (1, 'ada', 10);",
             "INSERT ada")) {
        return 1;
    }
    if (!Run(session, "INSERT INTO users(id, name, score) VALUES (2, 'bob', 30);",
             "INSERT bob")) {
        return 1;
    }
    if (!Run(session, "INSERT INTO users(id, name, score) VALUES (3, 'cy', 20);",
             "INSERT cy")) {
        return 1;
    }

    // Point lookup: planner chooses PkPointGet when WHERE is pk = const.
    if (!Run(session, "SELECT * FROM users WHERE id = 1;", "SELECT point")) return 1;

    // Residual filter: SeqScan + Filter when the predicate is not a pure PK eq.
    if (!Run(session, "SELECT name FROM users WHERE score > 15 ORDER BY name;",
             "SELECT filter+order")) {
        return 1;
    }

    if (!Run(session, "COMMIT;", "COMMIT")) return 1;

    // Autocommit: no open txn → single-statement begin/commit.
    if (!Run(session, "UPDATE users SET score = 99 WHERE id = 1;", "UPDATE (autocommit)")) {
        return 1;
    }
    if (!Run(session, "SELECT id, name, score FROM users WHERE id = 1;", "SELECT after update")) {
        return 1;
    }

    // DDL is rejected while a transaction is open.
    if (!Run(session, "BEGIN;", "BEGIN (ddl gate)")) return 1;
    st = session.Execute("CREATE TABLE other(id INT PRIMARY KEY);", result);
    if (st.ok()) {
        std::cerr << "expected DDL inside txn to fail\n";
        return 1;
    }
    std::cout << "== DDL inside txn (expected error)\n  " << st.ToString() << "\n";
    if (!Run(session, "ABORT;", "ABORT")) return 1;

    std::cout << "reldb_sql_example: ok\n";
    return 0;
}
