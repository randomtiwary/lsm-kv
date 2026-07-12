#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "lsmkv/status.h"
#include "reldb/database.h"
#include "reldb/expr.h"
#include "reldb/query_result.h"
#include "reldb/schema.h"
#include "reldb/sql_ast.h"
#include "reldb/txn.h"

namespace reldb {

class Executor;

// SQL frontend over reldb: parse → bind → rule-based plan → execute.
//
// Session state:
//   - Optional open transaction (BEGIN / COMMIT / ABORT).
//   - DML and SELECT outside a transaction use an auto-commit txn.
//   - CREATE TABLE is non-transactional and is rejected while InTransaction().
//
// Ownership: holds shared_ptr to Database; owns the open Transaction if any.
class SqlSession {
public:
    explicit SqlSession(std::shared_ptr<Database> db);

    // Parse and run one statement or a ';' script. result is the last statement.
    lsmkv::Status Execute(std::string_view sql, QueryResult& result);

    bool InTransaction() const { return txn_ != nullptr; }

private:
    lsmkv::Status RunStatement(Statement stmt, QueryResult& result);

    lsmkv::Status RunBegin(QueryResult& result);
    lsmkv::Status RunCommit(QueryResult& result);
    lsmkv::Status RunAbort(QueryResult& result);
    lsmkv::Status RunCreateTable(const CreateTableStmt& stmt, QueryResult& result);
    lsmkv::Status RunInsert(InsertStmt stmt, QueryResult& result);
    lsmkv::Status RunSelect(SelectStmt stmt, QueryResult& result);
    lsmkv::Status RunUpdate(UpdateStmt stmt, QueryResult& result);
    lsmkv::Status RunDelete(DeleteStmt stmt, QueryResult& result);

    // Opens txn_ for autocommit when none is open. Sets *used_auto.
    lsmkv::Status EnsureTxn(bool* used_auto);
    // After an auto-txn statement: Commit on success, Abort on failure; clear txn_.
    lsmkv::Status FinishAuto(const lsmkv::Status& op_st);

    lsmkv::Status LookupTable(const std::string& name, TableSchema* out) const;

    // Build a scan / point-get / range plan for table + optional WHERE residual.
    // On success *access_tag is e.g. "PkPointGet" / "PkRangeScan" / "SeqScan".
    // *filter may be set to residual WHERE for FilterExecutor (ownership transferred).
    lsmkv::Status PlanAccess(Transaction& txn, const TableSchema& schema,
                             std::unique_ptr<Expr> where, std::unique_ptr<Executor>* out,
                             std::string* access_tag);

    std::shared_ptr<Database> db_;
    std::unique_ptr<Transaction> txn_;
    bool auto_txn_ = false;
};

}  // namespace reldb
