#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "test_harness.h"
#include "test_util.h"

#include "reldb/database.h"
#include "reldb/executor.h"
#include "reldb/expr.h"
#include "reldb/query_result.h"
#include "reldb/row.h"
#include "reldb/schema.h"
#include "reldb/txn.h"
#include "reldb/types.h"

namespace {

reldb::TableSchema UsersSchema() {
    return reldb::TableSchema("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
        {"score", reldb::ColumnType::kInt64, false},
    });
}

reldb::Row User(std::int64_t id, const std::string& name, std::int64_t score) {
    return reldb::Row({reldb::Value::Int64(id), reldb::Value::String(name),
                       reldb::Value::Int64(score)});
}

std::shared_ptr<reldb::Database> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::shared_ptr<reldb::Database> db;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

// Seed users (1,ada,10), (2,bob,30), (3,cy,20) committed.
void SeedUsers(reldb::Database& db) {
    const auto schema = UsersSchema();
    EXPECT_OK(db.CreateTable(schema), "create");
    std::unique_ptr<reldb::Transaction> txn;
    EXPECT_OK(db.Begin(&txn), "begin seed");
    reldb::InsertOp ins(*txn, "users",
                        {User(1, "ada", 10), User(2, "bob", 30), User(3, "cy", 20)});
    reldb::QueryResult r;
    EXPECT_OK(ins.Execute(r), "seed insert");
    expect_eq(r.rows_affected, static_cast<std::uint64_t>(3), "seed n");
    EXPECT_OK(txn->Commit(), "seed commit");
}

}  // namespace

TEST(reldb_exec_seq_scan) {
    auto dir = MakeTempDir("reldb_exec_seq");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        SeedUsers(*db);

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        reldb::QueryResult r;
        {
            // Destroy the scan executor before the txn ends (iterator lock).
            auto scan = std::make_unique<reldb::SeqScanExecutor>(*txn, UsersSchema());
            EXPECT_OK(reldb::Collect(*scan, r), "collect");
        }
        expect_eq(r.plan_tag, std::string("SeqScan"), "tag");
        expect_eq(static_cast<int>(r.rows.size()), 3, "3 rows");
        expect_eq(r.column_names.size(), static_cast<std::size_t>(3), "3 cols");
        expect_eq(r.rows[0].at(0).GetInt64(), static_cast<std::int64_t>(1), "id1");
        expect_eq(r.rows[1].at(0).GetInt64(), static_cast<std::int64_t>(2), "id2");
        expect_eq(r.rows[2].at(0).GetInt64(), static_cast<std::int64_t>(3), "id3");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_pk_point_get) {
    auto dir = MakeTempDir("reldb_exec_point");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        SeedUsers(*db);

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        reldb::QueryResult r;
        {
            auto get = std::make_unique<reldb::PkPointGetExecutor>(
                *txn, UsersSchema(), reldb::Value::Int64(2));
            EXPECT_OK(reldb::Collect(*get, r), "collect");
        }
        expect_eq(r.plan_tag, std::string("PkPointGet"), "tag");
        expect_eq(static_cast<int>(r.rows.size()), 1, "one");
        expect_eq(r.rows[0].at(1).GetString(), std::string("bob"), "bob");

        {
            auto miss = std::make_unique<reldb::PkPointGetExecutor>(
                *txn, UsersSchema(), reldb::Value::Int64(99));
            EXPECT_OK(reldb::Collect(*miss, r), "miss");
        }
        expect_eq(static_cast<int>(r.rows.size()), 0, "empty");
        expect_eq(r.plan_tag, std::string("PkPointGet"), "tag miss");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_pk_range_scan) {
    auto dir = MakeTempDir("reldb_exec_range");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        SeedUsers(*db);

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        reldb::QueryResult r;
        {
            auto scan = std::make_unique<reldb::PkRangeScanExecutor>(
                *txn, UsersSchema(), reldb::Value::Int64(1), reldb::Value::Int64(3));
            EXPECT_OK(reldb::Collect(*scan, r), "collect");
        }
        expect_eq(r.plan_tag, std::string("PkRangeScan"), "tag");
        expect_eq(static_cast<int>(r.rows.size()), 2, "1,2");
        expect_eq(r.rows[0].at(0).GetInt64(), static_cast<std::int64_t>(1), "1");
        expect_eq(r.rows[1].at(0).GetInt64(), static_cast<std::int64_t>(2), "2");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_filter_project_limit) {
    auto dir = MakeTempDir("reldb_exec_fpl");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        SeedUsers(*db);
        const auto schema = UsersSchema();

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");

        // SELECT name FROM users WHERE score > 15 LIMIT 1
        // Plan: Limit<-Project<-Filter<-SeqScan
        reldb::QueryResult r;
        {
            auto pred = reldb::Expr::Compare(reldb::CmpOp::kGt, reldb::Expr::Column("score"),
                                             reldb::Expr::Literal(reldb::Value::Int64(15)));
            auto scan = std::make_unique<reldb::SeqScanExecutor>(*txn, schema);
            auto filter = std::make_unique<reldb::FilterExecutor>(std::move(scan), schema,
                                                                  std::move(pred));
            std::vector<reldb::Projection> projs;
            projs.push_back({"name", reldb::Expr::Column("name")});
            auto project = std::make_unique<reldb::ProjectExecutor>(std::move(filter), schema,
                                                                    std::move(projs));
            auto limit = std::make_unique<reldb::LimitExecutor>(std::move(project), 1);
            EXPECT_OK(reldb::Collect(*limit, r), "collect");
        }
        expect_eq(r.plan_tag, std::string("Limit<-Project<-Filter<-SeqScan"), "tag");
        expect_eq(static_cast<int>(r.rows.size()), 1, "limit 1");
        expect_eq(static_cast<int>(r.column_names.size()), 1, "1 col");
        expect_eq(r.column_names[0], std::string("name"), "name col");
        // Scan order is id 1,2,3; score>15 keeps bob(30) then cy(20); limit 1 → bob
        expect_eq(r.rows[0].at(0).GetString(), std::string("bob"), "bob");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_sort) {
    auto dir = MakeTempDir("reldb_exec_sort");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        SeedUsers(*db);
        const auto schema = UsersSchema();

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");

        reldb::QueryResult r;
        {
            auto scan = std::make_unique<reldb::SeqScanExecutor>(*txn, schema);
            std::vector<reldb::SortKey> keys = {{/*score*/ 2, true}};
            auto sort = std::make_unique<reldb::SortExecutor>(std::move(scan), std::move(keys));
            EXPECT_OK(reldb::Collect(*sort, r), "collect");
        }
        expect_eq(r.plan_tag, std::string("Sort<-SeqScan"), "tag");
        expect_eq(static_cast<int>(r.rows.size()), 3, "3");
        expect_eq(r.rows[0].at(2).GetInt64(), static_cast<std::int64_t>(10), "10");
        expect_eq(r.rows[1].at(2).GetInt64(), static_cast<std::int64_t>(20), "20");
        expect_eq(r.rows[2].at(2).GetInt64(), static_cast<std::int64_t>(30), "30");
        expect_eq(r.rows[0].at(1).GetString(), std::string("ada"), "ada");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_update_delete) {
    auto dir = MakeTempDir("reldb_exec_ud");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        SeedUsers(*db);
        const auto schema = UsersSchema();

        // UPDATE users SET score = 99 WHERE id = 1  via PkPointGet source
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin upd");
            auto src = std::make_unique<reldb::PkPointGetExecutor>(
                *txn, schema, reldb::Value::Int64(1));
            std::vector<reldb::Assignment> assigns;
            assigns.push_back({/*score*/ 2, reldb::Expr::Literal(reldb::Value::Int64(99))});
            reldb::UpdateOp upd(*txn, schema, std::move(src), std::move(assigns));
            reldb::QueryResult r;
            EXPECT_OK(upd.Execute(r), "update");
            expect_eq(r.rows_affected, static_cast<std::uint64_t>(1), "1 upd");
            expect_eq(r.plan_tag, std::string("Update<-PkPointGet"), "upd tag");
            EXPECT_OK(txn->Commit(), "commit upd");
        }

        // Verify + DELETE WHERE score >= 30 (bob 30, and ada 99)
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin del");
            reldb::Row row;
            EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &row), "get ada");
            expect_eq(row.at(2).GetInt64(), static_cast<std::int64_t>(99), "score 99");

            reldb::QueryResult r;
            {
                auto pred = reldb::Expr::Compare(
                    reldb::CmpOp::kGe, reldb::Expr::Column("score"),
                    reldb::Expr::Literal(reldb::Value::Int64(30)));
                auto scan = std::make_unique<reldb::SeqScanExecutor>(*txn, schema);
                auto filter = std::make_unique<reldb::FilterExecutor>(
                    std::move(scan), schema, std::move(pred));
                // DeleteOp materializes + resets source before writes.
                reldb::DeleteOp del(*txn, schema, std::move(filter));
                EXPECT_OK(del.Execute(r), "delete");
            }
            // bob score 30, ada 99 → both match; cy 20 stays
            expect_eq(r.rows_affected, static_cast<std::uint64_t>(2), "2 del");
            expect_eq(r.plan_tag, std::string("Delete<-Filter<-SeqScan"), "del tag");
            EXPECT_OK(txn->Commit(), "commit del");
        }

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin check");
            reldb::QueryResult r;
            {
                auto scan = std::make_unique<reldb::SeqScanExecutor>(*txn, schema);
                EXPECT_OK(reldb::Collect(*scan, r), "left");
            }
            expect_eq(static_cast<int>(r.rows.size()), 1, "only cy");
            expect_eq(r.rows[0].at(1).GetString(), std::string("cy"), "cy");
            EXPECT_OK(txn->Abort(), "abort");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_insert_and_filter_uncommitted) {
    // Read-your-writes: insert in same txn, then SeqScan+Filter sees the row.
    auto dir = MakeTempDir("reldb_exec_ryw");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        const auto schema = UsersSchema();
        EXPECT_OK(db->CreateTable(schema), "create");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        reldb::InsertOp ins(*txn, "users", {User(7, "grace", 5)});
        reldb::QueryResult r;
        EXPECT_OK(ins.Execute(r), "insert");
        expect_eq(r.rows_affected, static_cast<std::uint64_t>(1), "1");
        expect_eq(r.plan_tag, std::string("Insert"), "ins tag");

        {
            auto pred = reldb::Expr::Compare(reldb::CmpOp::kEq, reldb::Expr::Column("id"),
                                             reldb::Expr::Literal(reldb::Value::Int64(7)));
            auto scan = std::make_unique<reldb::SeqScanExecutor>(*txn, schema);
            auto filter = std::make_unique<reldb::FilterExecutor>(std::move(scan), schema,
                                                                  std::move(pred));
            EXPECT_OK(reldb::Collect(*filter, r), "filter");
        }
        expect_eq(static_cast<int>(r.rows.size()), 1, "sees insert");
        expect_eq(r.rows[0].at(1).GetString(), std::string("grace"), "grace");
        EXPECT_OK(txn->Commit(), "commit");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_update_rejects_pk_change) {
    auto dir = MakeTempDir("reldb_exec_pkchg");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        SeedUsers(*db);
        const auto schema = UsersSchema();

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        auto src = std::make_unique<reldb::PkPointGetExecutor>(*txn, schema,
                                                               reldb::Value::Int64(1));
        std::vector<reldb::Assignment> assigns;
        assigns.push_back({/*id PK*/ 0, reldb::Expr::Literal(reldb::Value::Int64(42))});
        reldb::UpdateOp upd(*txn, schema, std::move(src), std::move(assigns));
        reldb::QueryResult r;
        expect(upd.Execute(r).IsInvalidArgument(), "pk change rejected");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_hash_agg_scalar) {
    auto dir = MakeTempDir("reldb_exec_agg_scalar");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        SeedUsers(*db);
        const auto schema = UsersSchema();

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        reldb::QueryResult r;
        {
            auto scan = std::make_unique<reldb::SeqScanExecutor>(*txn, schema);
            // score is column index 2
            std::vector<reldb::AggSpec> aggs = {
                {reldb::AggFunc::kCount, true, -1, "cnt"},
                {reldb::AggFunc::kSum, false, 2, "sum_score"},
                {reldb::AggFunc::kAvg, false, 2, "avg_score"},
                {reldb::AggFunc::kMin, false, 2, "min_score"},
                {reldb::AggFunc::kMax, false, 2, "max_score"},
            };
            auto agg = std::make_unique<reldb::HashAggregateExecutor>(
                std::move(scan), /*group_by=*/std::vector<int>{}, std::move(aggs));
            EXPECT_OK(reldb::Collect(*agg, r), "collect");
        }
        expect_eq(r.plan_tag, std::string("HashAggregate<-SeqScan"), "tag");
        expect_eq(static_cast<int>(r.rows.size()), 1, "one row");
        expect_eq(r.rows[0].at(0).GetInt64(), static_cast<std::int64_t>(3), "count");
        // scores 10+30+20 = 60; avg 20
        expect_eq(r.rows[0].at(1).GetInt64(), static_cast<std::int64_t>(60), "sum");
        expect_eq(r.rows[0].at(2).GetInt64(), static_cast<std::int64_t>(20), "avg");
        expect_eq(r.rows[0].at(3).GetInt64(), static_cast<std::int64_t>(10), "min");
        expect_eq(r.rows[0].at(4).GetInt64(), static_cast<std::int64_t>(30), "max");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_hash_agg_empty_scalar) {
    auto dir = MakeTempDir("reldb_exec_agg_empty");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        reldb::QueryResult r;
        {
            auto scan = std::make_unique<reldb::SeqScanExecutor>(*txn, UsersSchema());
            std::vector<reldb::AggSpec> aggs = {
                {reldb::AggFunc::kCount, true, -1, "cnt"},
                {reldb::AggFunc::kSum, false, 2, "s"},
                {reldb::AggFunc::kMin, false, 2, "m"},
            };
            auto agg = std::make_unique<reldb::HashAggregateExecutor>(
                std::move(scan), std::vector<int>{}, std::move(aggs));
            EXPECT_OK(reldb::Collect(*agg, r), "collect");
        }
        expect_eq(static_cast<int>(r.rows.size()), 1, "one row");
        expect_eq(r.rows[0].at(0).GetInt64(), static_cast<std::int64_t>(0), "count0");
        expect(r.rows[0].at(1).IsNull(), "sum null");
        expect(r.rows[0].at(2).IsNull(), "min null");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_hash_agg_group_by) {
    auto dir = MakeTempDir("reldb_exec_agg_gb");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        // Two rows share name "ada".
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> seed;
            EXPECT_OK(db->Begin(&seed), "seed");
            reldb::InsertOp ins(*seed, "users",
                                {User(1, "ada", 10), User(2, "ada", 30), User(3, "bob", 20)});
            reldb::QueryResult r;
            EXPECT_OK(ins.Execute(r), "ins");
            EXPECT_OK(seed->Commit(), "c");
        }

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        reldb::QueryResult r;
        {
            auto scan = std::make_unique<reldb::SeqScanExecutor>(*txn, UsersSchema());
            // GROUP BY name (col 1), COUNT(*), SUM(score)
            std::vector<reldb::AggSpec> aggs = {
                {reldb::AggFunc::kCount, true, -1, "cnt"},
                {reldb::AggFunc::kSum, false, 2, "sum_score"},
            };
            auto agg = std::make_unique<reldb::HashAggregateExecutor>(
                std::move(scan), std::vector<int>{1}, std::move(aggs));
            EXPECT_OK(reldb::Collect(*agg, r), "collect");
        }
        expect_eq(r.plan_tag, std::string("HashAggregate<-SeqScan"), "tag");
        expect_eq(static_cast<int>(r.rows.size()), 2, "2 groups");
        // First-seen order: ada then bob
        expect_eq(r.rows[0].at(0).GetString(), std::string("ada"), "ada");
        expect_eq(r.rows[0].at(1).GetInt64(), static_cast<std::int64_t>(2), "ada cnt");
        expect_eq(r.rows[0].at(2).GetInt64(), static_cast<std::int64_t>(40), "ada sum");
        expect_eq(r.rows[1].at(0).GetString(), std::string("bob"), "bob");
        expect_eq(r.rows[1].at(1).GetInt64(), static_cast<std::int64_t>(1), "bob cnt");
        expect_eq(r.rows[1].at(2).GetInt64(), static_cast<std::int64_t>(20), "bob sum");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_hash_agg_empty_group_by) {
    auto dir = MakeTempDir("reldb_exec_agg_gb_empty");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        reldb::QueryResult r;
        {
            auto scan = std::make_unique<reldb::SeqScanExecutor>(*txn, UsersSchema());
            std::vector<reldb::AggSpec> aggs = {
                {reldb::AggFunc::kCount, true, -1, "cnt"},
            };
            auto agg = std::make_unique<reldb::HashAggregateExecutor>(
                std::move(scan), std::vector<int>{1}, std::move(aggs));
            EXPECT_OK(reldb::Collect(*agg, r), "collect");
        }
        expect_eq(static_cast<int>(r.rows.size()), 0, "zero groups");
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_exec_hash_agg_sum_overflow) {
    auto dir = MakeTempDir("reldb_exec_agg_ovf");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> seed;
            EXPECT_OK(db->Begin(&seed), "seed");
            reldb::InsertOp ins(
                *seed, "users",
                {User(1, "a", std::numeric_limits<std::int64_t>::max()), User(2, "b", 1)});
            reldb::QueryResult r;
            EXPECT_OK(ins.Execute(r), "ins");
            EXPECT_OK(seed->Commit(), "c");
        }

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        reldb::QueryResult r;
        {
            auto scan = std::make_unique<reldb::SeqScanExecutor>(*txn, UsersSchema());
            std::vector<reldb::AggSpec> aggs = {
                {reldb::AggFunc::kSum, false, 2, "s"},
            };
            auto agg = std::make_unique<reldb::HashAggregateExecutor>(
                std::move(scan), std::vector<int>{}, std::move(aggs));
            expect(reldb::Collect(*agg, r).IsInvalidArgument(), "overflow");
        }
        EXPECT_OK(txn->Abort(), "abort");
    }
    RemoveDirRecursive(dir);
}
