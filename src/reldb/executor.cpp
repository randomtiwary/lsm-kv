#include "reldb/executor.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lsmkv/debug.h"
#include "lsmkv/encoding.h"
#include "reldb/macros.h"
#include "reldb/row.h"

namespace reldb {
namespace {

std::vector<std::string> ColumnNamesFromSchema(const TableSchema& schema) {
    std::vector<std::string> names;
    names.reserve(schema.num_columns());
    for (const auto& col : schema.columns()) {
        names.push_back(col.name);
    }
    return names;
}

// Ordering for ORDER BY: NULLs sort last in both ASC and DESC; same-type only.
lsmkv::Status CompareForSort(const Value& a, const Value& b, int* out) {
    if (out == nullptr) return STATUS(InvalidArgument, "null cmp out");
    if (a.IsNull() && b.IsNull()) {
        *out = 0;
        return STATUS(OK);
    }
    if (a.IsNull()) {
        *out = 1;  // a after b
        return STATUS(OK);
    }
    if (b.IsNull()) {
        *out = -1;
        return STATUS(OK);
    }
    if (a.type() != b.type()) {
        return STATUS(InvalidArgument, "ORDER BY type mismatch");
    }
    switch (a.type()) {
        case ColumnType::kInt64:
            *out = (a.GetInt64() < b.GetInt64()) ? -1 : (a.GetInt64() > b.GetInt64() ? 1 : 0);
            return STATUS(OK);
        case ColumnType::kString:
            *out = a.GetString().compare(b.GetString());
            return STATUS(OK);
        case ColumnType::kBool: {
            const int av = a.GetBool() ? 1 : 0;
            const int bv = b.GetBool() ? 1 : 0;
            *out = av - bv;
            return STATUS(OK);
        }
        case ColumnType::kNull:
            *out = 0;
            return STATUS(OK);
    }
    return STATUS(InvalidArgument, "unknown value type in ORDER BY");
}

lsmkv::Status EnsureOpenable(bool* opened) {
    if (opened == nullptr) return STATUS(InvalidArgument, "null opened");
    if (*opened) return STATUS(InvalidArgument, "executor already opened");
    *opened = true;
    return STATUS(OK);
}

}  // namespace

// ---------------------------------------------------------------------------
// Collect
// ---------------------------------------------------------------------------

lsmkv::Status Collect(Executor& exec, QueryResult& result) {
    result.Clear();
    RELDB_RETURN_NOT_OK(exec.Open());
    result.column_names = exec.column_names();
    result.plan_tag = exec.PlanTag();
    bool has_row = false;
    for (;;) {
        RELDB_RETURN_NOT_OK(exec.Next(&has_row));
        if (!has_row) break;
        result.rows.push_back(exec.current_row());
    }
    return STATUS(OK);
}

// ---------------------------------------------------------------------------
// SeqScan
// ---------------------------------------------------------------------------

SeqScanExecutor::SeqScanExecutor(Transaction& txn, const TableSchema& schema)
    : txn_(txn), schema_(schema), column_names_(ColumnNamesFromSchema(schema_)) {}

lsmkv::Status SeqScanExecutor::Open() {
    RELDB_RETURN_NOT_OK(EnsureOpenable(&opened_));
    RELDB_RETURN_NOT_OK(txn_.Scan(schema_.name(), nullptr, nullptr, &scan_));
    return STATUS(OK);
}

lsmkv::Status SeqScanExecutor::Next(bool* has_row) {
    if (has_row == nullptr) return STATUS(InvalidArgument, "null has_row");
    if (!opened_) return STATUS(InvalidArgument, "executor not opened");
    if (scan_ == nullptr || !scan_->Valid()) {
        if (scan_ != nullptr) {
            RELDB_RETURN_NOT_OK(scan_->status());
            scan_.reset();  // release iterator locks once exhausted
        }
        *has_row = false;
        return STATUS(OK);
    }
    current_ = scan_->row();
    *has_row = true;
    scan_->Next();
    RELDB_RETURN_NOT_OK(scan_->status());
    return STATUS(OK);
}

const Row& SeqScanExecutor::current_row() const { return current_; }

// ---------------------------------------------------------------------------
// PkPointGet
// ---------------------------------------------------------------------------

PkPointGetExecutor::PkPointGetExecutor(Transaction& txn, const TableSchema& schema, Value pk)
    : txn_(txn),
      schema_(schema),
      pk_(std::move(pk)),
      column_names_(ColumnNamesFromSchema(schema_)) {}

lsmkv::Status PkPointGetExecutor::Open() {
    RELDB_RETURN_NOT_OK(EnsureOpenable(&opened_));
    auto st = txn_.Get(schema_.name(), pk_, &current_);
    if (st.IsNotFound()) {
        found_ = false;
        return STATUS(OK);
    }
    RELDB_RETURN_NOT_OK(st);
    found_ = true;
    return STATUS(OK);
}

lsmkv::Status PkPointGetExecutor::Next(bool* has_row) {
    if (has_row == nullptr) return STATUS(InvalidArgument, "null has_row");
    if (!opened_) return STATUS(InvalidArgument, "executor not opened");
    if (done_ || !found_) {
        *has_row = false;
        return STATUS(OK);
    }
    done_ = true;
    *has_row = true;
    return STATUS(OK);
}

const Row& PkPointGetExecutor::current_row() const { return current_; }

// ---------------------------------------------------------------------------
// PkRangeScan
// ---------------------------------------------------------------------------

PkRangeScanExecutor::PkRangeScanExecutor(Transaction& txn, const TableSchema& schema,
                                         std::optional<Value> start_pk,
                                         std::optional<Value> end_pk)
    : txn_(txn),
      schema_(schema),
      start_pk_(std::move(start_pk)),
      end_pk_(std::move(end_pk)),
      column_names_(ColumnNamesFromSchema(schema_)) {}

lsmkv::Status PkRangeScanExecutor::Open() {
    RELDB_RETURN_NOT_OK(EnsureOpenable(&opened_));
    const Value* start = start_pk_ ? &*start_pk_ : nullptr;
    const Value* end = end_pk_ ? &*end_pk_ : nullptr;
    RELDB_RETURN_NOT_OK(txn_.Scan(schema_.name(), start, end, &scan_));
    return STATUS(OK);
}

lsmkv::Status PkRangeScanExecutor::Next(bool* has_row) {
    if (has_row == nullptr) return STATUS(InvalidArgument, "null has_row");
    if (!opened_) return STATUS(InvalidArgument, "executor not opened");
    if (scan_ == nullptr || !scan_->Valid()) {
        if (scan_ != nullptr) {
            RELDB_RETURN_NOT_OK(scan_->status());
            scan_.reset();  // release iterator locks once exhausted
        }
        *has_row = false;
        return STATUS(OK);
    }
    current_ = scan_->row();
    *has_row = true;
    scan_->Next();
    RELDB_RETURN_NOT_OK(scan_->status());
    return STATUS(OK);
}

const Row& PkRangeScanExecutor::current_row() const { return current_; }

// ---------------------------------------------------------------------------
// Filter
// ---------------------------------------------------------------------------

FilterExecutor::FilterExecutor(std::unique_ptr<Executor> child, const TableSchema& input_schema,
                               std::unique_ptr<Expr> predicate)
    : child_(std::move(child)),
      input_schema_(input_schema),
      predicate_(std::move(predicate)) {}

lsmkv::Status FilterExecutor::Open() {
    if (child_ == nullptr || predicate_ == nullptr) {
        return STATUS(InvalidArgument, "null child or predicate");
    }
    RELDB_RETURN_NOT_OK(EnsureOpenable(&opened_));
    RELDB_RETURN_NOT_OK(predicate_->Bind(input_schema_));
    RELDB_RETURN_NOT_OK(child_->Open());
    return STATUS(OK);
}

lsmkv::Status FilterExecutor::Next(bool* has_row) {
    if (has_row == nullptr) return STATUS(InvalidArgument, "null has_row");
    if (!opened_) return STATUS(InvalidArgument, "executor not opened");
    for (;;) {
        bool child_has = false;
        RELDB_RETURN_NOT_OK(child_->Next(&child_has));
        if (!child_has) {
            *has_row = false;
            return STATUS(OK);
        }
        bool keep = false;
        RELDB_RETURN_NOT_OK(
            predicate_->EvalBool(child_->current_row(), input_schema_, &keep));
        if (keep) {
            *has_row = true;
            return STATUS(OK);
        }
    }
}

const Row& FilterExecutor::current_row() const { return child_->current_row(); }

const std::vector<std::string>& FilterExecutor::column_names() const {
    return child_->column_names();
}

std::string FilterExecutor::PlanTag() const {
    return "Filter<-" + (child_ ? child_->PlanTag() : std::string("?"));
}

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------

ProjectExecutor::ProjectExecutor(std::unique_ptr<Executor> child, const TableSchema& input_schema,
                                 std::vector<Projection> projections)
    : child_(std::move(child)),
      input_schema_(input_schema),
      projections_(std::move(projections)) {
    column_names_.reserve(projections_.size());
    for (const auto& p : projections_) {
        column_names_.push_back(p.name);
    }
}

lsmkv::Status ProjectExecutor::Open() {
    if (child_ == nullptr) return STATUS(InvalidArgument, "null child");
    if (projections_.empty()) return STATUS(InvalidArgument, "empty projection list");
    RELDB_RETURN_NOT_OK(EnsureOpenable(&opened_));
    for (auto& p : projections_) {
        if (p.expr == nullptr) return STATUS(InvalidArgument, "null projection expr");
        RELDB_RETURN_NOT_OK(p.expr->Bind(input_schema_));
    }
    RELDB_RETURN_NOT_OK(child_->Open());
    return STATUS(OK);
}

lsmkv::Status ProjectExecutor::Next(bool* has_row) {
    if (has_row == nullptr) return STATUS(InvalidArgument, "null has_row");
    if (!opened_) return STATUS(InvalidArgument, "executor not opened");
    bool child_has = false;
    RELDB_RETURN_NOT_OK(child_->Next(&child_has));
    if (!child_has) {
        *has_row = false;
        return STATUS(OK);
    }
    std::vector<Value> cells;
    cells.reserve(projections_.size());
    for (const auto& p : projections_) {
        Value v;
        RELDB_RETURN_NOT_OK(p.expr->Eval(child_->current_row(), input_schema_, &v));
        cells.push_back(std::move(v));
    }
    current_ = Row(std::move(cells));
    *has_row = true;
    return STATUS(OK);
}

const Row& ProjectExecutor::current_row() const { return current_; }

std::string ProjectExecutor::PlanTag() const {
    return "Project<-" + (child_ ? child_->PlanTag() : std::string("?"));
}

// ---------------------------------------------------------------------------
// Sort
// ---------------------------------------------------------------------------

SortExecutor::SortExecutor(std::unique_ptr<Executor> child, std::vector<SortKey> keys)
    : child_(std::move(child)), keys_(std::move(keys)) {}

lsmkv::Status SortExecutor::Open() {
    if (child_ == nullptr) return STATUS(InvalidArgument, "null child");
    if (keys_.empty()) return STATUS(InvalidArgument, "empty sort keys");
    RELDB_RETURN_NOT_OK(EnsureOpenable(&opened_));
    plan_tag_ = "Sort<-" + child_->PlanTag();
    RELDB_RETURN_NOT_OK(child_->Open());
    column_names_ = child_->column_names();

    bool has_row = false;
    for (;;) {
        RELDB_RETURN_NOT_OK(child_->Next(&has_row));
        if (!has_row) break;
        rows_.push_back(child_->current_row());
    }
    // Child fully drained; drop it so any underlying scan is released.
    child_.reset();

    // Stable multi-key sort. Comparator records the first error in sort_st.
    lsmkv::Status sort_st = STATUS(OK);
    const auto& keys = keys_;
    std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
        if (!sort_st.ok()) return false;
        for (const auto& key : keys) {
            if (key.column_index < 0 ||
                static_cast<std::size_t>(key.column_index) >= a.size() ||
                static_cast<std::size_t>(key.column_index) >= b.size()) {
                sort_st = STATUS(InvalidArgument, "ORDER BY column out of range");
                return false;
            }
            int cmp = 0;
            auto st = CompareForSort(a.at(static_cast<std::size_t>(key.column_index)),
                                     b.at(static_cast<std::size_t>(key.column_index)), &cmp);
            if (!st.ok()) {
                sort_st = st;
                return false;
            }
            if (cmp != 0) {
                return key.ascending ? (cmp < 0) : (cmp > 0);
            }
        }
        return false;  // equal
    });
    RELDB_RETURN_NOT_OK(sort_st);
    index_ = 0;
    return STATUS(OK);
}

lsmkv::Status SortExecutor::Next(bool* has_row) {
    if (has_row == nullptr) return STATUS(InvalidArgument, "null has_row");
    if (!opened_) return STATUS(InvalidArgument, "executor not opened");
    if (index_ >= rows_.size()) {
        *has_row = false;
        return STATUS(OK);
    }
    current_ = rows_[index_];
    ++index_;
    *has_row = true;
    return STATUS(OK);
}

const Row& SortExecutor::current_row() const { return current_; }

const std::vector<std::string>& SortExecutor::column_names() const { return column_names_; }

std::string SortExecutor::PlanTag() const {
    if (child_ != nullptr) {
        return "Sort<-" + child_->PlanTag();
    }
    return plan_tag_;
}

// ---------------------------------------------------------------------------
// Limit
// ---------------------------------------------------------------------------

LimitExecutor::LimitExecutor(std::unique_ptr<Executor> child, std::uint64_t limit)
    : child_(std::move(child)), limit_(limit) {}

lsmkv::Status LimitExecutor::Open() {
    if (child_ == nullptr) return STATUS(InvalidArgument, "null child");
    RELDB_RETURN_NOT_OK(EnsureOpenable(&opened_));
    RELDB_RETURN_NOT_OK(child_->Open());
    produced_ = 0;
    return STATUS(OK);
}

// ---------------------------------------------------------------------------
// Hash aggregate
// ---------------------------------------------------------------------------

namespace {

// *sum += delta with Int64 overflow check.
lsmkv::Status AddChecked(std::int64_t* sum, std::int64_t delta) {
    LSMKV_DCHECK(sum != nullptr);
    if ((delta > 0 && *sum > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && *sum < std::numeric_limits<std::int64_t>::min() - delta)) {
        return STATUS(InvalidArgument, "SUM overflow");
    }
    *sum += delta;
    return STATUS(OK);
}

// Stable encoding of a group key (length-prefixed cell encodings).
std::string EncodeGroupKey(const std::vector<Value>& parts) {
    std::string out;
    for (const auto& v : parts) {
        const std::string cell = Row::EncodeValue(v);
        lsmkv::PutFixed32(&out, static_cast<std::uint32_t>(cell.size()));
        out.append(cell);
    }
    return out;
}

struct AggAcc {
    std::int64_t n = 0;  // contributing rows (COUNT; AVG denominator)
    std::int64_t sum = 0;
    std::int64_t minv = 0;
    std::int64_t maxv = 0;
    bool has_numeric = false;  // saw at least one non-null numeric arg
};

struct GroupState {
    std::vector<Value> key_parts;
    std::vector<AggAcc> accs;
};

lsmkv::Status Accumulate(const AggSpec& spec, const Row& row, AggAcc* acc) {
    LSMKV_DCHECK(acc != nullptr);

    if (spec.func == AggFunc::kCount && spec.star) {
        ++acc->n;
        return STATUS(OK);
    }

    if (spec.arg_column < 0 ||
        static_cast<std::size_t>(spec.arg_column) >= row.size()) {
        return STATUS(InvalidArgument, "aggregate column out of range");
    }
    const Value& cell = row.at(static_cast<std::size_t>(spec.arg_column));

    if (spec.func == AggFunc::kCount) {
        // COUNT(col): skip nulls.
        if (!cell.IsNull()) ++acc->n;
        return STATUS(OK);
    }

    // SUM / AVG / MIN / MAX require Int64 (nulls skipped).
    if (cell.IsNull()) return STATUS(OK);
    if (cell.type() != ColumnType::kInt64) {
        return STATUS(InvalidArgument, "aggregate argument must be Int64");
    }
    const std::int64_t v = cell.GetInt64();
    if (spec.func == AggFunc::kSum || spec.func == AggFunc::kAvg) {
        RELDB_RETURN_NOT_OK(AddChecked(&acc->sum, v));
        ++acc->n;
        acc->has_numeric = true;
        return STATUS(OK);
    }
    if (spec.func == AggFunc::kMin) {
        if (!acc->has_numeric || v < acc->minv) acc->minv = v;
        acc->has_numeric = true;
        ++acc->n;
        return STATUS(OK);
    }
    if (spec.func == AggFunc::kMax) {
        if (!acc->has_numeric || v > acc->maxv) acc->maxv = v;
        acc->has_numeric = true;
        ++acc->n;
        return STATUS(OK);
    }
    // Only COUNT/SUM/AVG/MIN/MAX exist; invalid values are a programming error.
    LSMKV_DCHECK(false);
    return STATUS(InvalidArgument, "unknown aggregate function");
}

lsmkv::Status FinalizeAgg(const AggSpec& spec, const AggAcc& acc, Value* out) {
    LSMKV_DCHECK(out != nullptr);
    switch (spec.func) {
        case AggFunc::kCount:
            *out = Value::Int64(acc.n);
            return STATUS(OK);
        case AggFunc::kSum:
            if (!acc.has_numeric) {
                *out = Value::Null();
            } else {
                *out = Value::Int64(acc.sum);
            }
            return STATUS(OK);
        case AggFunc::kAvg:
            if (!acc.has_numeric || acc.n == 0) {
                *out = Value::Null();
            } else {
                // Truncating Int64 division toward zero.
                *out = Value::Int64(acc.sum / acc.n);
            }
            return STATUS(OK);
        case AggFunc::kMin:
            if (!acc.has_numeric) {
                *out = Value::Null();
            } else {
                *out = Value::Int64(acc.minv);
            }
            return STATUS(OK);
        case AggFunc::kMax:
            if (!acc.has_numeric) {
                *out = Value::Null();
            } else {
                *out = Value::Int64(acc.maxv);
            }
            return STATUS(OK);
    }
    LSMKV_DCHECK(false);
    return STATUS(InvalidArgument, "unknown aggregate function");
}

}  // namespace

HashAggregateExecutor::HashAggregateExecutor(std::unique_ptr<Executor> child,
                                             std::vector<int> group_by_columns,
                                             std::vector<AggSpec> aggs)
    : child_(std::move(child)),
      group_by_columns_(std::move(group_by_columns)),
      aggs_(std::move(aggs)) {}

lsmkv::Status HashAggregateExecutor::Open() {
    if (child_ == nullptr) return STATUS(InvalidArgument, "null child");
    if (aggs_.empty()) return STATUS(InvalidArgument, "empty aggregate list");
    for (const auto& a : aggs_) {
        if (a.output_name.empty()) {
            return STATUS(InvalidArgument, "aggregate output_name is empty");
        }
        if (a.star) {
            if (a.func != AggFunc::kCount) {
                return STATUS(InvalidArgument, "only COUNT(*) uses star");
            }
        } else if (a.arg_column < 0) {
            return STATUS(InvalidArgument, "aggregate arg_column is required");
        }
    }
    RELDB_RETURN_NOT_OK(EnsureOpenable(&opened_));
    plan_tag_ = "HashAggregate<-" + child_->PlanTag();
    RELDB_RETURN_NOT_OK(child_->Open());

    const auto& child_names = child_->column_names();
    column_names_.clear();
    column_names_.reserve(group_by_columns_.size() + aggs_.size());
    for (int idx : group_by_columns_) {
        if (idx < 0 || static_cast<std::size_t>(idx) >= child_names.size()) {
            return STATUS(InvalidArgument, "GROUP BY column out of range");
        }
        column_names_.push_back(child_names[static_cast<std::size_t>(idx)]);
    }
    for (const auto& a : aggs_) {
        column_names_.push_back(a.output_name);
    }

    std::unordered_map<std::string, GroupState> groups;
    std::vector<std::string> order;  // first-seen group key order

    bool has_row = false;
    for (;;) {
        RELDB_RETURN_NOT_OK(child_->Next(&has_row));
        if (!has_row) break;
        const Row& row = child_->current_row();

        std::vector<Value> key_parts;
        key_parts.reserve(group_by_columns_.size());
        for (int idx : group_by_columns_) {
            if (idx < 0 || static_cast<std::size_t>(idx) >= row.size()) {
                return STATUS(InvalidArgument, "GROUP BY column out of range");
            }
            key_parts.push_back(row.at(static_cast<std::size_t>(idx)));
        }
        const std::string key = EncodeGroupKey(key_parts);

        auto it = groups.find(key);
        if (it == groups.end()) {
            GroupState st;
            st.key_parts = std::move(key_parts);
            st.accs.assign(aggs_.size(), AggAcc{});
            order.push_back(key);
            it = groups.emplace(key, std::move(st)).first;
        }
        for (std::size_t i = 0; i < aggs_.size(); ++i) {
            RELDB_RETURN_NOT_OK(Accumulate(aggs_[i], row, &it->second.accs[i]));
        }
    }
    // Child fully drained; drop it so any underlying scan is released.
    child_.reset();

    if (groups.empty()) {
        if (group_by_columns_.empty()) {
            // Scalar aggregate on empty input: one result row.
            std::vector<Value> cells;
            cells.reserve(aggs_.size());
            for (const auto& a : aggs_) {
                AggAcc empty;
                Value v;
                RELDB_RETURN_NOT_OK(FinalizeAgg(a, empty, &v));
                cells.push_back(std::move(v));
            }
            rows_.push_back(Row(std::move(cells)));
        }
        // With GROUP BY and no input: zero rows.
        index_ = 0;
        return STATUS(OK);
    }

    rows_.reserve(order.size());
    for (const auto& key : order) {
        const GroupState& st = groups.at(key);
        std::vector<Value> cells;
        cells.reserve(st.key_parts.size() + aggs_.size());
        for (const auto& k : st.key_parts) {
            cells.push_back(k);
        }
        for (std::size_t i = 0; i < aggs_.size(); ++i) {
            Value v;
            RELDB_RETURN_NOT_OK(FinalizeAgg(aggs_[i], st.accs[i], &v));
            cells.push_back(std::move(v));
        }
        rows_.push_back(Row(std::move(cells)));
    }
    index_ = 0;
    return STATUS(OK);
}

lsmkv::Status HashAggregateExecutor::Next(bool* has_row) {
    if (has_row == nullptr) return STATUS(InvalidArgument, "null has_row");
    if (!opened_) return STATUS(InvalidArgument, "executor not opened");
    if (index_ >= rows_.size()) {
        *has_row = false;
        return STATUS(OK);
    }
    current_ = rows_[index_];
    ++index_;
    *has_row = true;
    return STATUS(OK);
}

const Row& HashAggregateExecutor::current_row() const { return current_; }

std::string HashAggregateExecutor::PlanTag() const {
    if (child_ != nullptr) {
        return "HashAggregate<-" + child_->PlanTag();
    }
    return plan_tag_;
}

// ---------------------------------------------------------------------------
// Limit (continued)
// ---------------------------------------------------------------------------

lsmkv::Status LimitExecutor::Next(bool* has_row) {
    if (has_row == nullptr) return STATUS(InvalidArgument, "null has_row");
    if (!opened_) return STATUS(InvalidArgument, "executor not opened");
    if (produced_ >= limit_) {
        *has_row = false;
        return STATUS(OK);
    }
    bool child_has = false;
    RELDB_RETURN_NOT_OK(child_->Next(&child_has));
    if (!child_has) {
        *has_row = false;
        return STATUS(OK);
    }
    ++produced_;
    *has_row = true;
    return STATUS(OK);
}

const Row& LimitExecutor::current_row() const { return child_->current_row(); }

const std::vector<std::string>& LimitExecutor::column_names() const {
    return child_->column_names();
}

std::string LimitExecutor::PlanTag() const {
    return "Limit<-" + (child_ ? child_->PlanTag() : std::string("?"));
}

// ---------------------------------------------------------------------------
// InsertOp
// ---------------------------------------------------------------------------

InsertOp::InsertOp(Transaction& txn, std::string table, std::vector<Row> rows)
    : txn_(txn), table_(std::move(table)), rows_(std::move(rows)) {}

lsmkv::Status InsertOp::Execute(QueryResult& result) {
    result.Clear();
    result.plan_tag = PlanTag();
    for (const auto& row : rows_) {
        RELDB_RETURN_NOT_OK(txn_.Insert(table_, row));
        ++result.rows_affected;
    }
    return STATUS(OK);
}

// ---------------------------------------------------------------------------
// UpdateOp
// ---------------------------------------------------------------------------

UpdateOp::UpdateOp(Transaction& txn, const TableSchema& schema, std::unique_ptr<Executor> source,
                   std::vector<Assignment> assignments)
    : txn_(txn),
      schema_(schema),
      source_(std::move(source)),
      assignments_(std::move(assignments)) {}

std::string UpdateOp::PlanTag() const {
    return "Update<-" + (source_ ? source_->PlanTag() : std::string("?"));
}

lsmkv::Status UpdateOp::Execute(QueryResult& result) {
    if (source_ == nullptr) return STATUS(InvalidArgument, "null source");
    if (assignments_.empty()) return STATUS(InvalidArgument, "empty assignments");

    // Capture plan tag before materializing (source stays alive until reset).
    const std::string tag = PlanTag();

    RELDB_RETURN_NOT_OK(source_->Open());
    for (const auto& a : assignments_) {
        if (a.expr == nullptr) return STATUS(InvalidArgument, "null assignment expr");
        if (a.column_index < 0 ||
            static_cast<std::size_t>(a.column_index) >= schema_.num_columns()) {
            return STATUS(InvalidArgument, "assignment column out of range");
        }
        // Bind mutates Expr::column_index_; unique_ptr is non-const via get().
        RELDB_RETURN_NOT_OK(a.expr->Bind(schema_));
    }

    std::vector<Row> targets;
    bool has_row = false;
    for (;;) {
        RELDB_RETURN_NOT_OK(source_->Next(&has_row));
        if (!has_row) break;
        targets.push_back(source_->current_row());
    }
    // Destroy scan before writes (Transaction::Scan contract).
    source_.reset();

    result.Clear();
    result.plan_tag = tag;
    for (const auto& old_row : targets) {
        Row new_row = old_row;
        for (const auto& a : assignments_) {
            Value v;
            RELDB_RETURN_NOT_OK(a.expr->Eval(old_row, schema_, &v));
            new_row.Set(static_cast<std::size_t>(a.column_index), std::move(v));
        }
        Value old_pk;
        Value new_pk;
        RELDB_RETURN_NOT_OK(old_row.PrimaryKey(schema_, &old_pk));
        RELDB_RETURN_NOT_OK(new_row.PrimaryKey(schema_, &new_pk));
        if (old_pk != new_pk) {
            return STATUS(InvalidArgument, "UPDATE must not change primary key");
        }
        RELDB_RETURN_NOT_OK(txn_.Update(schema_.name(), new_row));
        ++result.rows_affected;
    }
    return STATUS(OK);
}

// ---------------------------------------------------------------------------
// DeleteOp
// ---------------------------------------------------------------------------

DeleteOp::DeleteOp(Transaction& txn, const TableSchema& schema, std::unique_ptr<Executor> source)
    : txn_(txn), schema_(schema), source_(std::move(source)) {}

std::string DeleteOp::PlanTag() const {
    return "Delete<-" + (source_ ? source_->PlanTag() : std::string("?"));
}

lsmkv::Status DeleteOp::Execute(QueryResult& result) {
    if (source_ == nullptr) return STATUS(InvalidArgument, "null source");

    const std::string tag = PlanTag();
    RELDB_RETURN_NOT_OK(source_->Open());

    std::vector<Value> pks;
    bool has_row = false;
    for (;;) {
        RELDB_RETURN_NOT_OK(source_->Next(&has_row));
        if (!has_row) break;
        Value pk;
        RELDB_RETURN_NOT_OK(source_->current_row().PrimaryKey(schema_, &pk));
        pks.push_back(std::move(pk));
    }
    source_.reset();

    result.Clear();
    result.plan_tag = tag;
    for (const auto& pk : pks) {
        RELDB_RETURN_NOT_OK(txn_.Delete(schema_.name(), pk));
        ++result.rows_affected;
    }
    return STATUS(OK);
}

}  // namespace reldb
