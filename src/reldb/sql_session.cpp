#include "reldb/sql_session.h"

#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "reldb/executor.h"
#include "lsmkv/debug.h"
#include "reldb/macros.h"
#include "reldb/sql_parser.h"
#include "reldb/types.h"

namespace reldb {
namespace {

// True if expr is Column(pk_name) = Literal(v) or Literal(v) = Column(pk_name).
bool MatchPkEquality(const Expr& e, const std::string& pk_name, Value* out_pk) {
    if (e.kind() != Expr::Kind::kCompare || e.cmp_op() != CmpOp::kEq) return false;
    const Expr* left = e.left();
    const Expr* right = e.right();
    if (left == nullptr || right == nullptr) return false;

    auto pick = [&](const Expr& col, const Expr& lit) -> bool {
        if (col.kind() != Expr::Kind::kColumn || lit.kind() != Expr::Kind::kLiteral) return false;
        if (col.column_name() != pk_name) return false;
        if (lit.literal().IsNull()) return false;
        *out_pk = lit.literal();
        return true;
    };
    return pick(*left, *right) || pick(*right, *left);
}

// True if expr is Column(pk) <op> Literal or Literal <op> Column(pk).
bool MatchPkCompare(const Expr& e, const std::string& pk_name, bool* pk_on_left, CmpOp* op,
                    Value* bound) {
    if (e.kind() != Expr::Kind::kCompare) return false;
    const Expr* left = e.left();
    const Expr* right = e.right();
    if (left == nullptr || right == nullptr) return false;

    if (left->kind() == Expr::Kind::kColumn && right->kind() == Expr::Kind::kLiteral &&
        left->column_name() == pk_name && !right->literal().IsNull()) {
        *pk_on_left = true;
        *op = e.cmp_op();
        *bound = right->literal();
        return true;
    }
    if (right->kind() == Expr::Kind::kColumn && left->kind() == Expr::Kind::kLiteral &&
        right->column_name() == pk_name && !left->literal().IsNull()) {
        *pk_on_left = false;
        *op = e.cmp_op();
        *bound = left->literal();
        return true;
    }
    return false;
}

// Flip comparison so column is on the left: 5 < id → id > 5.
CmpOp FlipCmp(CmpOp op) {
    switch (op) {
        case CmpOp::kEq: return CmpOp::kEq;
        case CmpOp::kNe: return CmpOp::kNe;
        case CmpOp::kLt: return CmpOp::kGt;
        case CmpOp::kLe: return CmpOp::kGe;
        case CmpOp::kGt: return CmpOp::kLt;
        case CmpOp::kGe: return CmpOp::kLe;
    }
    return op;
}

// Half-open [start, end) from a column-on-left PK comparison.
// Only ops representable without next-key encoding: pk >= c, pk < c.
bool BoundsFromPkCmp(CmpOp op_col_left, const Value& bound, std::optional<Value>* start,
                     std::optional<Value>* end) {
    start->reset();
    end->reset();
    switch (op_col_left) {
        case CmpOp::kGe:
            *start = bound;
            return true;
        case CmpOp::kLt:
            *end = bound;
            return true;
        default:
            return false;
    }
}

std::string PkColumnName(const TableSchema& schema) {
    const int idx = schema.primary_key_index();
    if (idx < 0) return {};
    return schema.columns()[static_cast<std::size_t>(idx)].name;
}

}  // namespace

// RAII for statement-level autocommit.
// Ensure() begins a txn when the session has none. Complete(op_st) commits on
// success or aborts on failure. If Complete is skipped (early return / exception),
// the destructor aborts the auto-txn so we never leave a dangling one open.
class SqlSession::AutoTxnGuard {
public:
    explicit AutoTxnGuard(SqlSession& session) : session_(session) {}

    AutoTxnGuard(const AutoTxnGuard&) = delete;
    AutoTxnGuard& operator=(const AutoTxnGuard&) = delete;

    ~AutoTxnGuard() {
        if (!active_) return;
        session_.auto_txn_ = false;
        if (session_.txn_ != nullptr) {
            (void)session_.txn_->Abort();
            session_.txn_.reset();
        }
    }

    // Begin a session txn if needed; marks this guard as responsible for finishing it.
    lsmkv::Status Ensure() {
        if (session_.txn_ != nullptr) return STATUS(OK);
        RELDB_RETURN_NOT_OK(session_.db_->Begin(&session_.txn_));
        session_.auto_txn_ = true;
        active_ = true;
        return STATUS(OK);
    }

    // Finish the auto-txn (if any). Must be used on every intentional return path
    // after Ensure; otherwise the destructor aborts.
    lsmkv::Status Complete(const lsmkv::Status& op_st) {
        if (!active_) return op_st;
        active_ = false;
        session_.auto_txn_ = false;
        if (op_st.ok()) {
            const auto cst = session_.txn_->Commit();
            session_.txn_.reset();
            RELDB_RETURN_NOT_OK(cst);
            return op_st;
        }
        (void)session_.txn_->Abort();
        session_.txn_.reset();
        return op_st;
    }

private:
    SqlSession& session_;
    bool active_ = false;
};

SqlSession::SqlSession(std::shared_ptr<Database> db) : db_(std::move(db)) {
    LSMKV_DCHECK(db_ != nullptr);
}

lsmkv::Status SqlSession::Execute(std::string_view sql, QueryResult& result) {
    LSMKV_DCHECK(db_ != nullptr);
    result.Clear();
    std::vector<Statement> stmts;
    RELDB_RETURN_NOT_OK(ParseScript(sql, &stmts));
    if (stmts.empty()) return STATUS(OK);
    for (auto& stmt : stmts) {
        RELDB_RETURN_NOT_OK(RunStatement(std::move(stmt), result));
    }
    return STATUS(OK);
}

lsmkv::Status SqlSession::LookupTable(const std::string& name, TableSchema* out) const {
    LSMKV_DCHECK(out != nullptr);
    return db_->GetTable(name, out);
}

lsmkv::Status SqlSession::RunStatement(Statement stmt, QueryResult& result) {
    return std::visit(
        [this, &result](auto&& node) -> lsmkv::Status {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, BeginStmt>) {
                return RunBegin(result);
            } else if constexpr (std::is_same_v<T, CommitStmt>) {
                return RunCommit(result);
            } else if constexpr (std::is_same_v<T, AbortStmt>) {
                return RunAbort(result);
            } else if constexpr (std::is_same_v<T, CreateTableStmt>) {
                return RunCreateTable(node, result);
            } else if constexpr (std::is_same_v<T, DropTableStmt>) {
                return RunDropTable(node, result);
            } else if constexpr (std::is_same_v<T, AlterTableAddColumnStmt>) {
                return RunAlterTableAddColumn(node, result);
            } else if constexpr (std::is_same_v<T, AlterTableDropColumnStmt>) {
                return RunAlterTableDropColumn(node, result);
            } else if constexpr (std::is_same_v<T, InsertStmt>) {
                return RunInsert(std::move(node), result);
            } else if constexpr (std::is_same_v<T, SelectStmt>) {
                return RunSelect(std::move(node), result);
            } else if constexpr (std::is_same_v<T, UpdateStmt>) {
                return RunUpdate(std::move(node), result);
            } else if constexpr (std::is_same_v<T, DeleteStmt>) {
                return RunDelete(std::move(node), result);
            }
        },
        std::move(stmt));
}

lsmkv::Status SqlSession::RunBegin(QueryResult& result) {
    result.Clear();
    RELDB_FAIL_IF(txn_ != nullptr, InvalidArgument, "already in a transaction");
    RELDB_RETURN_NOT_OK(db_->Begin(&txn_));
    auto_txn_ = false;
    return STATUS(OK);
}

lsmkv::Status SqlSession::RunCommit(QueryResult& result) {
    result.Clear();
    RELDB_FAIL_IF_NOT_IN_TRANSACTION(txn_);
    auto_txn_ = false;
    const auto st = txn_->Commit();
    txn_.reset();
    return st;
}

lsmkv::Status SqlSession::RunAbort(QueryResult& result) {
    result.Clear();
    RELDB_FAIL_IF_NOT_IN_TRANSACTION(txn_);
    auto_txn_ = false;
    const auto st = txn_->Abort();
    txn_.reset();
    return st;
}

lsmkv::Status SqlSession::RunCreateTable(const CreateTableStmt& stmt, QueryResult& result) {
    result.Clear();
    RELDB_FAIL_IF(InTransaction(), InvalidArgument, "DDL is not allowed inside a transaction");
    std::vector<ColumnDef> cols;
    cols.reserve(stmt.columns.size());
    for (const auto& c : stmt.columns) {
        cols.push_back(ColumnDef{c.name, c.type, c.primary_key});
    }
    TableSchema schema(stmt.table_name, std::move(cols));
    RELDB_RETURN_NOT_OK(schema.Validate());
    RELDB_RETURN_NOT_OK(db_->CreateTable(schema));
    return STATUS(OK);
}

lsmkv::Status SqlSession::RunDropTable(const DropTableStmt& stmt, QueryResult& result) {
    result.Clear();
    RELDB_FAIL_IF(InTransaction(), InvalidArgument, "DDL is not allowed inside a transaction");
    auto st = db_->DropTable(stmt.table_name);
    // Plain DROP TABLE → NotFound if missing. IF EXISTS → success (no-op).
    if (stmt.if_exists && st.IsNotFound()) {
        return STATUS(OK);
    }
    return st;
}

lsmkv::Status SqlSession::RunAlterTableAddColumn(const AlterTableAddColumnStmt& stmt,
                                                 QueryResult& result) {
    result.Clear();
    RELDB_FAIL_IF(InTransaction(), InvalidArgument, "DDL is not allowed inside a transaction");
    ColumnDef col{stmt.column.name, stmt.column.type, /*primary_key=*/false};
    return db_->AlterTableAddColumn(stmt.table_name, col, stmt.default_value);
}

lsmkv::Status SqlSession::RunAlterTableDropColumn(const AlterTableDropColumnStmt& stmt,
                                                  QueryResult& result) {
    result.Clear();
    RELDB_FAIL_IF(InTransaction(), InvalidArgument, "DDL is not allowed inside a transaction");
    return db_->AlterTableDropColumn(stmt.table_name, stmt.column_name);
}

lsmkv::Status SqlSession::RunInsert(InsertStmt stmt, QueryResult& result) {
    TableSchema schema;
    RELDB_RETURN_NOT_OK(LookupTable(stmt.table_name, &schema));

    Row row;
    if (stmt.column_names.empty()) {
        RELDB_FAIL_IF(stmt.values.size() != schema.num_columns(), InvalidArgument,
                      "INSERT value count does not match table width");
        row = Row(std::move(stmt.values));
    } else {
        RELDB_FAIL_IF(stmt.column_names.size() != stmt.values.size(), InvalidArgument,
                      "INSERT column count does not match VALUES count");
        std::vector<Value> cells(schema.num_columns(), Value::Null());
        std::vector<bool> set(schema.num_columns(), false);
        for (std::size_t i = 0; i < stmt.column_names.size(); ++i) {
            const int idx = schema.ColumnIndex(stmt.column_names[i]);
            RELDB_FAIL_IF(idx < 0, InvalidArgument, "unknown column: " + stmt.column_names[i]);
            const auto ui = static_cast<std::size_t>(idx);
            RELDB_FAIL_IF(set[ui], InvalidArgument, "duplicate column in INSERT: " + stmt.column_names[i]);
            cells[ui] = std::move(stmt.values[i]);
            set[ui] = true;
        }
        for (std::size_t i = 0; i < set.size(); ++i) {
            RELDB_FAIL_IF(!set[i], InvalidArgument,
                          "missing column in INSERT: " + schema.columns()[i].name);
        }
        row = Row(std::move(cells));
    }
    RELDB_RETURN_NOT_OK(row.ValidateAgainst(schema));

    AutoTxnGuard auto_txn(*this);
    RELDB_RETURN_NOT_OK(auto_txn.Ensure());
    InsertOp op(*txn_, schema.name(), {std::move(row)});
    return auto_txn.Complete(op.Execute(result));
}

lsmkv::Status SqlSession::PlanAccess(Transaction& txn, const TableSchema& schema,
                                     std::unique_ptr<Expr> where,
                                     std::unique_ptr<Executor>* out, std::string* access_tag) {
    LSMKV_DCHECK(out != nullptr);
    LSMKV_DCHECK(access_tag != nullptr);
    const std::string pk_name = PkColumnName(schema);
    RELDB_FAIL_IF(pk_name.empty(), InvalidArgument, "table has no primary key");

    if (where != nullptr) {
        Value pk;
        if (MatchPkEquality(*where, pk_name, &pk)) {
            RELDB_RETURN_NOT_OK(where->Bind(schema));
            *access_tag = "PkPointGet";
            *out = std::make_unique<PkPointGetExecutor>(txn, schema, std::move(pk));
            return STATUS(OK);
        }

        bool pk_on_left = true;
        CmpOp op = CmpOp::kEq;
        Value bound;
        if (MatchPkCompare(*where, pk_name, &pk_on_left, &op, &bound)) {
            const CmpOp col_op = pk_on_left ? op : FlipCmp(op);
            std::optional<Value> start;
            std::optional<Value> end;
            if (BoundsFromPkCmp(col_op, bound, &start, &end)) {
                RELDB_RETURN_NOT_OK(where->Bind(schema));
                *access_tag = "PkRangeScan";
                *out = std::make_unique<PkRangeScanExecutor>(txn, schema, std::move(start),
                                                             std::move(end));
                return STATUS(OK);
            }
        }
    }

    *access_tag = "SeqScan";
    std::unique_ptr<Executor> scan = std::make_unique<SeqScanExecutor>(txn, schema);
    if (where != nullptr) {
        RELDB_RETURN_NOT_OK(where->Bind(schema));
        *out = std::make_unique<FilterExecutor>(std::move(scan), schema, std::move(where));
    } else {
        *out = std::move(scan);
    }
    return STATUS(OK);
}

lsmkv::Status SqlSession::RunSelect(SelectStmt stmt, QueryResult& result) {
    TableSchema schema;
    RELDB_RETURN_NOT_OK(LookupTable(stmt.from.table_name, &schema));

    // Aggregates / GROUP BY / HAVING land in C1–C4; reject if present early.
    RELDB_FAIL_IF(!stmt.group_by.empty(), InvalidArgument, "GROUP BY is not supported");
    RELDB_FAIL_IF(stmt.having != nullptr, InvalidArgument, "HAVING is not supported");

    AutoTxnGuard auto_txn(*this);
    RELDB_RETURN_NOT_OK(auto_txn.Ensure());

    std::unique_ptr<Executor> plan;
    std::string access_tag;
    RELDB_RETURN_NOT_OK(
        PlanAccess(*txn_, schema, std::move(stmt.where), &plan, &access_tag));

    // Projection names (for ORDER BY after Project).
    std::vector<std::string> out_names;
    if (!stmt.select_star) {
        RELDB_RETURN_IF(stmt.select_list.empty(),
                        auto_txn.Complete(STATUS(InvalidArgument, "empty SELECT list")));
        std::vector<Projection> projs;
        projs.reserve(stmt.select_list.size());
        out_names.reserve(stmt.select_list.size());
        for (auto& item : stmt.select_list) {
            RELDB_RETURN_IF(item.kind != SelectItem::Kind::kExpr,
                            auto_txn.Complete(STATUS(InvalidArgument,
                                                     "aggregates are not supported")));
            RELDB_RETURN_IF(item.expr == nullptr,
                            auto_txn.Complete(STATUS(InvalidArgument, "null select expression")));
            auto& e = item.expr;
            std::string name =
                (e->kind() == Expr::Kind::kColumn) ? e->column_name() : e->ToString();
            RELDB_RETURN_NOT_OK(e->Bind(schema));
            out_names.push_back(name);
            projs.push_back(Projection{std::move(name), std::move(e)});
        }
        plan = std::make_unique<ProjectExecutor>(std::move(plan), schema, std::move(projs));
    } else {
        for (const auto& c : schema.columns()) {
            out_names.push_back(c.name);
        }
    }

    if (!stmt.order_by.empty()) {
        std::vector<SortKey> keys;
        keys.reserve(stmt.order_by.size());
        for (const auto& o : stmt.order_by) {
            int idx = -1;
            for (std::size_t i = 0; i < out_names.size(); ++i) {
                if (out_names[i] == o.column_name) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
            RELDB_RETURN_IF(idx < 0,
                            auto_txn.Complete(STATUS(InvalidArgument,
                                                     "unknown ORDER BY column: " + o.column_name)));
            keys.push_back(SortKey{idx, o.ascending});
        }
        plan = std::make_unique<SortExecutor>(std::move(plan), std::move(keys));
    }

    if (stmt.has_limit) {
        RELDB_RETURN_IF(stmt.limit < 0,
                        auto_txn.Complete(STATUS(InvalidArgument, "LIMIT must be non-negative")));
        plan = std::make_unique<LimitExecutor>(std::move(plan),
                                               static_cast<std::uint64_t>(stmt.limit));
    }

    return auto_txn.Complete(Collect(*plan, result));
}

lsmkv::Status SqlSession::RunUpdate(UpdateStmt stmt, QueryResult& result) {
    TableSchema schema;
    RELDB_RETURN_NOT_OK(LookupTable(stmt.table_name, &schema));

    std::vector<Assignment> assigns;
    assigns.reserve(stmt.sets.size());
    for (auto& a : stmt.sets) {
        const int idx = schema.ColumnIndex(a.column_name);
        RELDB_FAIL_IF(idx < 0, InvalidArgument, "unknown column: " + a.column_name);
        RELDB_FAIL_IF(a.value == nullptr, InvalidArgument, "null assignment expression");
        RELDB_RETURN_NOT_OK(a.value->Bind(schema));
        assigns.push_back(Assignment{idx, std::move(a.value)});
    }

    AutoTxnGuard auto_txn(*this);
    RELDB_RETURN_NOT_OK(auto_txn.Ensure());

    std::unique_ptr<Executor> source;
    std::string access_tag;
    RELDB_RETURN_NOT_OK(
        PlanAccess(*txn_, schema, std::move(stmt.where), &source, &access_tag));

    UpdateOp op(*txn_, schema, std::move(source), std::move(assigns));
    return auto_txn.Complete(op.Execute(result));
}

lsmkv::Status SqlSession::RunDelete(DeleteStmt stmt, QueryResult& result) {
    TableSchema schema;
    RELDB_RETURN_NOT_OK(LookupTable(stmt.table_name, &schema));

    AutoTxnGuard auto_txn(*this);
    RELDB_RETURN_NOT_OK(auto_txn.Ensure());

    std::unique_ptr<Executor> source;
    std::string access_tag;
    RELDB_RETURN_NOT_OK(
        PlanAccess(*txn_, schema, std::move(stmt.where), &source, &access_tag));

    DeleteOp op(*txn_, schema, std::move(source));
    return auto_txn.Complete(op.Execute(result));
}

}  // namespace reldb
