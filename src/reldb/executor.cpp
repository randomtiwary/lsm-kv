#include "reldb/executor.h"

#include <algorithm>
#include <utility>

#include "reldb/macros.h"

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

lsmkv::Status Collect(Executor* exec, QueryResult* result) {
    if (exec == nullptr || result == nullptr) {
        return STATUS(InvalidArgument, "null exec or result");
    }
    result->Clear();
    RELDB_RETURN_NOT_OK(exec->Open());
    result->column_names = exec->column_names();
    result->plan_tag = exec->PlanTag();
    bool has_row = false;
    for (;;) {
        RELDB_RETURN_NOT_OK(exec->Next(&has_row));
        if (!has_row) break;
        result->rows.push_back(exec->current_row());
    }
    return STATUS(OK);
}

// ---------------------------------------------------------------------------
// SeqScan
// ---------------------------------------------------------------------------

SeqScanExecutor::SeqScanExecutor(Transaction* txn, TableSchema schema)
    : txn_(txn), schema_(std::move(schema)), column_names_(ColumnNamesFromSchema(schema_)) {}

lsmkv::Status SeqScanExecutor::Open() {
    if (txn_ == nullptr) return STATUS(InvalidArgument, "null txn");
    RELDB_RETURN_NOT_OK(EnsureOpenable(&opened_));
    RELDB_RETURN_NOT_OK(txn_->Scan(schema_.name(), nullptr, nullptr, &scan_));
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

PkPointGetExecutor::PkPointGetExecutor(Transaction* txn, TableSchema schema, Value pk)
    : txn_(txn),
      schema_(std::move(schema)),
      pk_(std::move(pk)),
      column_names_(ColumnNamesFromSchema(schema_)) {}

lsmkv::Status PkPointGetExecutor::Open() {
    if (txn_ == nullptr) return STATUS(InvalidArgument, "null txn");
    RELDB_RETURN_NOT_OK(EnsureOpenable(&opened_));
    auto st = txn_->Get(schema_.name(), pk_, &current_);
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

PkRangeScanExecutor::PkRangeScanExecutor(Transaction* txn, TableSchema schema,
                                         const Value* start_pk, const Value* end_pk)
    : txn_(txn),
      schema_(std::move(schema)),
      column_names_(ColumnNamesFromSchema(schema_)) {
    if (start_pk != nullptr) {
        has_start_ = true;
        start_pk_ = *start_pk;
    }
    if (end_pk != nullptr) {
        has_end_ = true;
        end_pk_ = *end_pk;
    }
}

lsmkv::Status PkRangeScanExecutor::Open() {
    if (txn_ == nullptr) return STATUS(InvalidArgument, "null txn");
    RELDB_RETURN_NOT_OK(EnsureOpenable(&opened_));
    const Value* start = has_start_ ? &start_pk_ : nullptr;
    const Value* end = has_end_ ? &end_pk_ : nullptr;
    RELDB_RETURN_NOT_OK(txn_->Scan(schema_.name(), start, end, &scan_));
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

FilterExecutor::FilterExecutor(std::unique_ptr<Executor> child, TableSchema input_schema,
                               std::unique_ptr<Expr> predicate)
    : child_(std::move(child)),
      input_schema_(std::move(input_schema)),
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

ProjectExecutor::ProjectExecutor(std::unique_ptr<Executor> child, TableSchema input_schema,
                                 std::vector<Projection> projections)
    : child_(std::move(child)),
      input_schema_(std::move(input_schema)),
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
    // After Open, child PlanTag may still be valid; keep the pre-Open snapshot.
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

InsertOp::InsertOp(Transaction* txn, std::string table, std::vector<Row> rows)
    : txn_(txn), table_(std::move(table)), rows_(std::move(rows)) {}

lsmkv::Status InsertOp::Execute(QueryResult* result) {
    if (txn_ == nullptr || result == nullptr) {
        return STATUS(InvalidArgument, "null txn or result");
    }
    result->Clear();
    result->plan_tag = PlanTag();
    for (const auto& row : rows_) {
        RELDB_RETURN_NOT_OK(txn_->Insert(table_, row));
        ++result->rows_affected;
    }
    return STATUS(OK);
}

// ---------------------------------------------------------------------------
// UpdateOp
// ---------------------------------------------------------------------------

UpdateOp::UpdateOp(Transaction* txn, TableSchema schema, std::unique_ptr<Executor> source,
                   std::vector<Assignment> assignments)
    : txn_(txn),
      schema_(std::move(schema)),
      source_(std::move(source)),
      assignments_(std::move(assignments)) {}

std::string UpdateOp::PlanTag() const {
    return "Update<-" + (source_ ? source_->PlanTag() : std::string("?"));
}

lsmkv::Status UpdateOp::Execute(QueryResult* result) {
    if (txn_ == nullptr || result == nullptr) {
        return STATUS(InvalidArgument, "null txn or result");
    }
    if (source_ == nullptr) return STATUS(InvalidArgument, "null source");
    if (assignments_.empty()) return STATUS(InvalidArgument, "empty assignments");

    // Capture plan tag before materializing (source stays alive until reset).
    const std::string tag = PlanTag();

    RELDB_RETURN_NOT_OK(source_->Open());
    for (auto& a : assignments_) {
        if (a.expr == nullptr) return STATUS(InvalidArgument, "null assignment expr");
        if (a.column_index < 0 ||
            static_cast<std::size_t>(a.column_index) >= schema_.num_columns()) {
            return STATUS(InvalidArgument, "assignment column out of range");
        }
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

    result->Clear();
    result->plan_tag = tag;
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
        RELDB_RETURN_NOT_OK(txn_->Update(schema_.name(), new_row));
        ++result->rows_affected;
    }
    return STATUS(OK);
}

// ---------------------------------------------------------------------------
// DeleteOp
// ---------------------------------------------------------------------------

DeleteOp::DeleteOp(Transaction* txn, TableSchema schema, std::unique_ptr<Executor> source)
    : txn_(txn), schema_(std::move(schema)), source_(std::move(source)) {}

std::string DeleteOp::PlanTag() const {
    return "Delete<-" + (source_ ? source_->PlanTag() : std::string("?"));
}

lsmkv::Status DeleteOp::Execute(QueryResult* result) {
    if (txn_ == nullptr || result == nullptr) {
        return STATUS(InvalidArgument, "null txn or result");
    }
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

    result->Clear();
    result->plan_tag = tag;
    for (const auto& pk : pks) {
        RELDB_RETURN_NOT_OK(txn_->Delete(schema_.name(), pk));
        ++result->rows_affected;
    }
    return STATUS(OK);
}

}  // namespace reldb
