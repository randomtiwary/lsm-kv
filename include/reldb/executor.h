#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lsmkv/status.h"
#include "reldb/expr.h"
#include "reldb/query_result.h"
#include "reldb/row.h"
#include "reldb/schema.h"
#include "reldb/txn.h"
#include "reldb/types.h"

namespace reldb {

// Physical operators for the SQL layer (volcano / pull model).
//
// SELECT-style operators implement Executor: Open once, then Next until
// *has_row is false. DML operators (Insert / Update / Delete) are one-shot
// Execute() classes — they materialize any child scan first so the underlying
// TableRowScan is destroyed before writes (required by Transaction::Scan).
//
// Ownership:
//   - Transaction& is non-owning; the txn must outlive the executor.
//   - Children and expression trees are owned via unique_ptr.
//   - TableSchema is accepted by const reference and copied into the operator
//     (small; name() is the table name for access paths).
//
// Scan lifetime: destroy scan-bearing executors (or fully exhaust them) before
// Commit / Abort / further writes on the same transaction. TableRowScan may
// hold DB iterator locks that block writers. UpdateOp / DeleteOp / SortExecutor
// already materialize and drop their child for this reason.
//
// PlanTag() composes child tags for educational EXPLAIN-style checks, e.g.
//   "Limit<-Filter<-SeqScan".

// ---------------------------------------------------------------------------
// Row-producing (volcano) operators
// ---------------------------------------------------------------------------

class Executor {
public:
    virtual ~Executor() = default;

    // Prepare resources (open scans, bind expressions). Safe to call once;
    // calling again after a successful Open is InvalidArgument.
    virtual lsmkv::Status Open() = 0;

    // Produce the next output row. *has_row is true when current_row() is valid.
    // At end of stream *has_row is false and current_row() is undefined.
    virtual lsmkv::Status Next(bool* has_row) = 0;

    // Valid only after Next returned has_row == true.
    virtual const Row& current_row() const = 0;

    // Output column labels (for QueryResult / projection).
    virtual const std::vector<std::string>& column_names() const = 0;

    // Access-path / operator label; includes child chain when nested.
    virtual std::string PlanTag() const = 0;
};

// Drain a plan into a QueryResult (column_names, rows, plan_tag).
lsmkv::Status Collect(Executor& exec, QueryResult& result);

// Full-table scan in PK / KV key order. Uses Transaction::Scan(table, null, null).
class SeqScanExecutor : public Executor {
public:
    SeqScanExecutor(Transaction& txn, const TableSchema& schema);

    lsmkv::Status Open() override;
    lsmkv::Status Next(bool* has_row) override;
    const Row& current_row() const override;
    const std::vector<std::string>& column_names() const override { return column_names_; }
    std::string PlanTag() const override { return "SeqScan"; }

private:
    Transaction& txn_;
    TableSchema schema_;
    std::vector<std::string> column_names_;
    std::unique_ptr<TableRowScan> scan_;
    bool opened_ = false;
    Row current_;
};

// Primary-key point lookup via Transaction::Get. Yields 0 or 1 row.
class PkPointGetExecutor : public Executor {
public:
    PkPointGetExecutor(Transaction& txn, const TableSchema& schema, Value pk);

    lsmkv::Status Open() override;
    lsmkv::Status Next(bool* has_row) override;
    const Row& current_row() const override;
    const std::vector<std::string>& column_names() const override { return column_names_; }
    std::string PlanTag() const override { return "PkPointGet"; }

private:
    Transaction& txn_;
    TableSchema schema_;
    Value pk_;
    std::vector<std::string> column_names_;
    bool opened_ = false;
    bool done_ = false;
    bool found_ = false;
    Row current_;
};

// PK range scan: start inclusive, end exclusive (nullopt bounds = unbounded).
// Matches Transaction::Scan half-open semantics.
class PkRangeScanExecutor : public Executor {
public:
    PkRangeScanExecutor(Transaction& txn, const TableSchema& schema,
                        std::optional<Value> start_pk, std::optional<Value> end_pk);

    lsmkv::Status Open() override;
    lsmkv::Status Next(bool* has_row) override;
    const Row& current_row() const override;
    const std::vector<std::string>& column_names() const override { return column_names_; }
    std::string PlanTag() const override { return "PkRangeScan"; }

private:
    Transaction& txn_;
    TableSchema schema_;
    std::optional<Value> start_pk_;
    std::optional<Value> end_pk_;
    std::vector<std::string> column_names_;
    std::unique_ptr<TableRowScan> scan_;
    bool opened_ = false;
    Row current_;
};

// Residual filter: keep rows where predicate EvalBool is true.
// input_schema describes child's row layout for expression evaluation.
class FilterExecutor : public Executor {
public:
    FilterExecutor(std::unique_ptr<Executor> child, const TableSchema& input_schema,
                   std::unique_ptr<Expr> predicate);

    lsmkv::Status Open() override;
    lsmkv::Status Next(bool* has_row) override;
    const Row& current_row() const override;
    const std::vector<std::string>& column_names() const override;
    std::string PlanTag() const override;

private:
    std::unique_ptr<Executor> child_;
    TableSchema input_schema_;
    std::unique_ptr<Expr> predicate_;
    bool opened_ = false;
};

// Projection list item: output column name + expression over the child row.
struct Projection {
    std::string name;
    std::unique_ptr<Expr> expr;
};

class ProjectExecutor : public Executor {
public:
    ProjectExecutor(std::unique_ptr<Executor> child, const TableSchema& input_schema,
                    std::vector<Projection> projections);

    lsmkv::Status Open() override;
    lsmkv::Status Next(bool* has_row) override;
    const Row& current_row() const override;
    const std::vector<std::string>& column_names() const override { return column_names_; }
    std::string PlanTag() const override;

private:
    std::unique_ptr<Executor> child_;
    TableSchema input_schema_;
    std::vector<Projection> projections_;
    std::vector<std::string> column_names_;
    bool opened_ = false;
    Row current_;
};

// ORDER BY column_index (into child row). In-memory materializing sort.
struct SortKey {
    int column_index = 0;
    bool ascending = true;
};

class SortExecutor : public Executor {
public:
    SortExecutor(std::unique_ptr<Executor> child, std::vector<SortKey> keys);

    lsmkv::Status Open() override;
    lsmkv::Status Next(bool* has_row) override;
    const Row& current_row() const override;
    const std::vector<std::string>& column_names() const override;
    std::string PlanTag() const override;

private:
    std::unique_ptr<Executor> child_;
    std::vector<SortKey> keys_;
    bool opened_ = false;
    std::vector<Row> rows_;
    std::size_t index_ = 0;  // next row to yield
    std::vector<std::string> column_names_;
    std::string plan_tag_ = "Sort";  // set in Open before child is released
    Row current_;
};

// LIMIT n — stop after n rows from the child.
class LimitExecutor : public Executor {
public:
    LimitExecutor(std::unique_ptr<Executor> child, std::uint64_t limit);

    lsmkv::Status Open() override;
    lsmkv::Status Next(bool* has_row) override;
    const Row& current_row() const override;
    const std::vector<std::string>& column_names() const override;
    std::string PlanTag() const override;

private:
    std::unique_ptr<Executor> child_;
    std::uint64_t limit_;
    std::uint64_t produced_ = 0;
    bool opened_ = false;
};

// ---------------------------------------------------------------------------
// DML (one-shot) operators
// ---------------------------------------------------------------------------

// INSERT one or more rows. Sets result.rows_affected.
class InsertOp {
public:
    InsertOp(Transaction& txn, std::string table, std::vector<Row> rows);

    lsmkv::Status Execute(QueryResult& result);
    std::string PlanTag() const { return "Insert"; }

private:
    Transaction& txn_;
    std::string table_;
    std::vector<Row> rows_;
};

// Column assignment for UPDATE: set columns_[col_index] = eval(expr).
struct Assignment {
    int column_index = 0;
    std::unique_ptr<Expr> expr;
};

// UPDATE rows produced by source. Materializes the child fully before any write.
// Assignments are evaluated against the old row; PK column must not change
// (enforced by comparing PrimaryKey before/after).
class UpdateOp {
public:
    UpdateOp(Transaction& txn, const TableSchema& schema, std::unique_ptr<Executor> source,
             std::vector<Assignment> assignments);

    lsmkv::Status Execute(QueryResult& result);
    std::string PlanTag() const;

private:
    Transaction& txn_;
    TableSchema schema_;
    std::unique_ptr<Executor> source_;
    std::vector<Assignment> assignments_;
};

// DELETE rows produced by source. Materializes the child fully before any write.
class DeleteOp {
public:
    DeleteOp(Transaction& txn, const TableSchema& schema, std::unique_ptr<Executor> source);

    lsmkv::Status Execute(QueryResult& result);
    std::string PlanTag() const;

private:
    Transaction& txn_;
    TableSchema schema_;
    std::unique_ptr<Executor> source_;
};

}  // namespace reldb
