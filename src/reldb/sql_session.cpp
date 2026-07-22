#include "reldb/sql_session.h"

#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "reldb/bind_context.h"
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

namespace {

// Resolve aggregate column arg to bare name + physical index (COUNT(*) → empty/-1).
lsmkv::Status ResolveAggArg(const BindContext& ctx, bool star, const std::string& col,
                            std::string* bare_col, int* arg_column) {
    if (star) {
        bare_col->clear();
        *arg_column = -1;
        return STATUS(OK);
    }
    BoundColumn bc;
    RELDB_RETURN_NOT_OK(ctx.Resolve(col, &bc));
    *bare_col = bc.column_name;
    // Single-table today: column_index == row_offset; multi-table join exec is later.
    *arg_column = bc.column_index;
    return STATUS(OK);
}

void CollectColumnNames(const Expr* e, std::vector<std::string>* out) {
    if (e == nullptr || out == nullptr) return;
    if (e->kind() == Expr::Kind::kColumn) {
        out->push_back(e->column_name());
        return;
    }
    CollectColumnNames(e->left(), out);
    CollectColumnNames(e->right(), out);
}

// Add AggSpec if not already present under the canonical default result name
// (SUM(u.score) and SUM(score) both become SUM(score) after Resolve).
lsmkv::Status EnsureAggSpec(const BindContext& ctx, AggFunc func, bool star,
                            const std::string& col, std::vector<AggSpec>* aggs,
                            std::string* canonical_name_out = nullptr) {
    std::string bare_col;
    int arg_column = -1;
    RELDB_RETURN_NOT_OK(ResolveAggArg(ctx, star, col, &bare_col, &arg_column));
    const std::string name = DefaultAggResultName(func, star, bare_col);
    if (canonical_name_out != nullptr) *canonical_name_out = name;
    for (const auto& a : *aggs) {
        if (a.output_name == name) return STATUS(OK);
    }
    AggSpec spec;
    spec.func = func;
    spec.star = star;
    spec.output_name = name;
    if (!star) {
        spec.arg_column = arg_column;
    }
    aggs->push_back(std::move(spec));
    return STATUS(OK);
}

// Rewrite HAVING column leaves so they match HashAggregate / agg_schema names:
// group keys → bare column; default agg forms → canonical SUM(score) not SUM(u.score).
lsmkv::Status NormalizeHavingColumns(Expr* having, const BindContext& ctx,
                                     const std::vector<std::string>& group_bare) {
    if (having == nullptr) return STATUS(OK);
    return having->MapColumnNames([&](std::string* name) -> lsmkv::Status {
        AggFunc f = AggFunc::kCount;
        bool star = false;
        std::string col;
        if (ParseDefaultAggResultName(*name, &f, &star, &col)) {
            std::string bare;
            int arg = -1;
            RELDB_RETURN_NOT_OK(ResolveAggArg(ctx, star, col, &bare, &arg));
            *name = DefaultAggResultName(f, star, bare);
            return STATUS(OK);
        }
        // Already a bare group key?
        for (const auto& g : group_bare) {
            if (g == *name) return STATUS(OK);
        }
        // Qualified group ref (u.name) → bare name.
        BoundColumn bc;
        if (ctx.Resolve(*name, &bc).ok()) {
            for (const auto& g : group_bare) {
                if (g == bc.column_name) {
                    *name = bc.column_name;
                    return STATUS(OK);
                }
            }
        }
        // Leave other names for Bind(agg_schema) / error path.
        return STATUS(OK);
    });
}

// ORDER BY: exact match on output labels first (includes AS aliases).
// Bare-name fallback only for select items that projected a column without AS
// (avoids SELECT name AS id … ORDER BY u.id matching the alias).
int FindOutputColumn(const std::vector<std::string>& out_names,
                     const std::vector<bool>& bare_fallback_ok, const std::string& name,
                     const BindContext& ctx) {
    for (std::size_t i = 0; i < out_names.size(); ++i) {
        if (out_names[i] == name) return static_cast<int>(i);
    }
    BoundColumn bc;
    if (!ctx.Resolve(name, &bc).ok()) return -1;
    for (std::size_t i = 0; i < out_names.size(); ++i) {
        if (i < bare_fallback_ok.size() && bare_fallback_ok[i] &&
            out_names[i] == bc.column_name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

lsmkv::Status SqlSession::RunSelect(SelectStmt stmt, QueryResult& result) {
    // Multi-table joins parse; execution is not wired yet.
    RELDB_FAIL_IF(!stmt.from.joins.empty(), InvalidArgument, "joins are not supported");

    TableSchema schema;
    RELDB_RETURN_NOT_OK(LookupTable(stmt.from.base.table_name, &schema));

    BindContext bind_ctx;
    RELDB_RETURN_NOT_OK(
        bind_ctx.AddTable(stmt.from.base.table_name, stmt.from.base.alias, schema));

    bool has_agg = false;
    for (const auto& item : stmt.select_list) {
        if (item.kind == SelectItem::Kind::kAgg) {
            has_agg = true;
            break;
        }
    }
    const bool has_group = !stmt.group_by.empty();
    const bool agg_plan = has_agg || has_group;

    if (stmt.select_star && agg_plan) {
        return STATUS(InvalidArgument, "SELECT * cannot be combined with aggregates or GROUP BY");
    }
    if (stmt.having != nullptr && !agg_plan) {
        return STATUS(InvalidArgument, "HAVING requires aggregates or GROUP BY");
    }

    // Normalize qualified column refs (u.id → id) before PK access-path matching.
    if (stmt.where != nullptr) {
        RELDB_RETURN_NOT_OK(stmt.where->Bind(bind_ctx));
    }

    AutoTxnGuard auto_txn(*this);
    RELDB_RETURN_NOT_OK(auto_txn.Ensure());

    std::unique_ptr<Executor> plan;
    std::string access_tag;
    RELDB_RETURN_NOT_OK(
        PlanAccess(*txn_, schema, std::move(stmt.where), &plan, &access_tag));

    // Projection / output names (for ORDER BY after the final Project).
    std::vector<std::string> out_names;
    // True when ORDER BY qual.col may match this output via bare column name.
    std::vector<bool> out_bare_fallback;

    if (!agg_plan) {
        // Non-aggregate SELECT (existing path).
        if (!stmt.select_star) {
            RELDB_RETURN_IF(stmt.select_list.empty(),
                            auto_txn.Complete(STATUS(InvalidArgument, "empty SELECT list")));
            std::vector<Projection> projs;
            projs.reserve(stmt.select_list.size());
            out_names.reserve(stmt.select_list.size());
            out_bare_fallback.reserve(stmt.select_list.size());
            for (auto& item : stmt.select_list) {
                RELDB_RETURN_IF(item.kind != SelectItem::Kind::kExpr,
                                auto_txn.Complete(STATUS(InvalidArgument,
                                                         "unexpected aggregate")));
                RELDB_RETURN_IF(item.expr == nullptr,
                                auto_txn.Complete(STATUS(InvalidArgument,
                                                         "null select expression")));
                auto& e = item.expr;
                RELDB_RETURN_NOT_OK(e->Bind(bind_ctx));
                std::string name;
                bool bare_ok = false;
                if (!item.alias.empty()) {
                    name = item.alias;
                } else if (e->kind() == Expr::Kind::kColumn) {
                    name = e->column_name();  // bare after Bind(bind_ctx)
                    bare_ok = true;
                } else {
                    name = e->ToString();
                }
                out_names.push_back(name);
                out_bare_fallback.push_back(bare_ok);
                projs.push_back(Projection{std::move(name), std::move(e)});
            }
            plan = std::make_unique<ProjectExecutor>(std::move(plan), schema, std::move(projs));
        } else {
            out_names = bind_ctx.StarOutputNames();
            out_bare_fallback.assign(out_names.size(), true);
        }
    } else {
        // Aggregate / GROUP BY path:
        //   access → HashAggregate → [Filter HAVING] → Project → Sort → Limit
        RELDB_RETURN_IF(stmt.select_list.empty(),
                        auto_txn.Complete(STATUS(InvalidArgument, "empty SELECT list")));

        // Resolve GROUP BY columns → indices into the input schema (bare names).
        std::vector<int> group_indices;
        std::vector<std::string> group_bare;
        group_indices.reserve(stmt.group_by.size());
        group_bare.reserve(stmt.group_by.size());
        for (const auto& col : stmt.group_by) {
            BoundColumn bc;
            auto st = bind_ctx.Resolve(col, &bc);
            RELDB_RETURN_IF(!st.ok(), auto_txn.Complete(st));
            group_indices.push_back(bc.column_index);
            group_bare.push_back(bc.column_name);
        }

        // Bind select list; collect unique AggSpecs by default result name.
        std::vector<AggSpec> aggs;
        // For each select item: source column name in the HashAggregate output.
        std::vector<std::string> select_sources;
        select_sources.reserve(stmt.select_list.size());
        out_names.reserve(stmt.select_list.size());
        out_bare_fallback.reserve(stmt.select_list.size());

        for (auto& item : stmt.select_list) {
            if (item.kind == SelectItem::Kind::kAgg) {
                std::string canonical;
                {
                    auto st = EnsureAggSpec(bind_ctx, item.agg_func, item.agg_star,
                                           item.agg_column, &aggs, &canonical);
                    if (!st.ok()) return auto_txn.Complete(st);
                }
                select_sources.push_back(canonical);
                out_names.push_back(item.alias.empty() ? canonical : item.alias);
                out_bare_fallback.push_back(false);
                continue;
            }

            // Non-aggregate: must be a column that appears in GROUP BY.
            RELDB_RETURN_IF(item.expr == nullptr,
                            auto_txn.Complete(STATUS(InvalidArgument, "null select expression")));
            RELDB_RETURN_IF(item.expr->kind() != Expr::Kind::kColumn,
                            auto_txn.Complete(STATUS(
                                InvalidArgument,
                                "SELECT with aggregates requires GROUP BY columns or aggregates")));
            BoundColumn bc;
            {
                auto st = bind_ctx.Resolve(item.expr->column_name(), &bc);
                if (!st.ok()) return auto_txn.Complete(st);
            }
            bool in_group = false;
            for (const auto& g : group_bare) {
                if (g == bc.column_name) {
                    in_group = true;
                    break;
                }
            }
            // Also accept an exact match on the original GROUP BY text.
            if (!in_group) {
                for (const auto& g : stmt.group_by) {
                    if (g == item.expr->column_name()) {
                        in_group = true;
                        break;
                    }
                }
            }
            RELDB_RETURN_IF(!in_group,
                            auto_txn.Complete(STATUS(
                                InvalidArgument,
                                "column must appear in GROUP BY: " + item.expr->column_name())));
            const std::string out =
                !item.alias.empty() ? item.alias : bc.column_name;
            select_sources.push_back(bc.column_name);  // matches HashAggregate / scan names
            out_names.push_back(out);
            out_bare_fallback.push_back(item.alias.empty());
        }

        // HAVING may reference aggregates not listed in the SELECT list; add them.
        // Normalize qualified group keys and SUM(u.col) → SUM(col) before Bind.
        if (stmt.having != nullptr) {
            {
                auto st = NormalizeHavingColumns(stmt.having.get(), bind_ctx, group_bare);
                if (!st.ok()) return auto_txn.Complete(st);
            }
            std::vector<std::string> having_cols;
            CollectColumnNames(stmt.having.get(), &having_cols);
            for (const auto& cname : having_cols) {
                bool is_group = false;
                for (const auto& g : group_bare) {
                    if (g == cname) {
                        is_group = true;
                        break;
                    }
                }
                if (is_group) continue;
                AggFunc f;
                bool star = false;
                std::string col;
                if (!ParseDefaultAggResultName(cname, &f, &star, &col)) {
                    // Not a group key and not a default agg name (e.g. AS alias).
                    return auto_txn.Complete(STATUS(
                        InvalidArgument,
                        "HAVING column must be a GROUP BY column or aggregate "
                        "(use COUNT(*)/SUM(col), not SELECT aliases): " +
                            cname));
                }
                {
                    auto st = EnsureAggSpec(bind_ctx, f, star, col, &aggs);
                    if (!st.ok()) return auto_txn.Complete(st);
                }
            }
        }

        // Synthetic schema matching HashAggregate output: group keys then unique aggs.
        std::vector<ColumnDef> agg_cols;
        bool saw_pk = false;
        for (std::size_t gi = 0; gi < group_bare.size(); ++gi) {
            const int gidx = group_indices[gi];
            ColumnDef c = schema.columns()[static_cast<std::size_t>(gidx)];
            c.primary_key = !saw_pk;
            saw_pk = true;
            agg_cols.push_back(std::move(c));
        }
        for (const auto& a : aggs) {
            ColumnDef c{a.output_name, ColumnType::kInt64, !saw_pk};
            saw_pk = true;
            agg_cols.push_back(std::move(c));
        }
        if (agg_cols.empty()) {
            return auto_txn.Complete(STATUS(InvalidArgument, "empty SELECT list"));
        }
        TableSchema agg_schema(schema.name(), std::move(agg_cols));
        RELDB_RETURN_NOT_OK(agg_schema.Validate());

        plan = std::make_unique<HashAggregateExecutor>(std::move(plan), std::move(group_indices),
                                                       aggs);

        if (stmt.having != nullptr) {
            RELDB_RETURN_NOT_OK(stmt.having->Bind(agg_schema));
            plan = std::make_unique<FilterExecutor>(std::move(plan), agg_schema,
                                                    std::move(stmt.having));
        }

        // Project into select-list order / aliases (multiple items may share one agg).
        std::vector<Projection> projs;
        projs.reserve(select_sources.size());
        for (std::size_t i = 0; i < select_sources.size(); ++i) {
            auto e = Expr::Column(select_sources[i]);
            RELDB_RETURN_NOT_OK(e->Bind(agg_schema));
            projs.push_back(Projection{out_names[i], std::move(e)});
        }
        plan = std::make_unique<ProjectExecutor>(std::move(plan), agg_schema, std::move(projs));
    }

    if (!stmt.order_by.empty()) {
        std::vector<SortKey> keys;
        keys.reserve(stmt.order_by.size());
        for (const auto& o : stmt.order_by) {
            const int idx =
                FindOutputColumn(out_names, out_bare_fallback, o.column_name, bind_ctx);
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
