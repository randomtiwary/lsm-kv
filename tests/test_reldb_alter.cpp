#include <memory>
#include <string>

#include "test_harness.h"
#include "test_util.h"

#include "lsmkv/options.h"
#include "reldb/database.h"
#include "reldb/row.h"
#include "reldb/schema.h"
#include "reldb/txn.h"
#include "reldb/types.h"

namespace {

reldb::TableSchema UsersSchema() {
    return reldb::TableSchema("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
    });
}

std::shared_ptr<reldb::Database> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::shared_ptr<reldb::Database> db;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

}  // namespace

TEST(reldb_alter_add_column_empty_table) {
    auto dir = MakeTempDir("reldb_alter_empty");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        reldb::ColumnDef age{"age", reldb::ColumnType::kInt64, false};
        EXPECT_OK(db->AlterTableAddColumn("users", age, reldb::Value::Int64(0)), "alter");

        reldb::TableSchema got;
        EXPECT_OK(db->GetTable("users", &got), "get");
        expect_eq(got.num_columns(), static_cast<std::size_t>(3), "ncols");
        expect_eq(got.columns()[2].name, std::string("age"), "age col");
        expect(!got.columns()[2].primary_key, "not pk");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_alter_add_column_rewrites_rows) {
    auto dir = MakeTempDir("reldb_alter_data");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("a")})),
                      "i1");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(2),
                                                       reldb::Value::String("b")})),
                      "i2");
            EXPECT_OK(txn->Commit(), "c");
        }

        reldb::ColumnDef age{"age", reldb::ColumnType::kInt64, false};
        EXPECT_OK(db->AlterTableAddColumn("users", age, reldb::Value::Int64(42)), "alter");

        reldb::TableSchema got;
        EXPECT_OK(db->GetTable("users", &got), "schema");
        expect_eq(got.num_columns(), static_cast<std::size_t>(3), "ncols");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin2");
            reldb::Row row;
            EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &row), "g1");
            expect_eq(row.size(), static_cast<std::size_t>(3), "w1");
            expect_eq(row.at(2).GetInt64(), static_cast<std::int64_t>(42), "def1");
            EXPECT_OK(txn->Get("users", reldb::Value::Int64(2), &row), "g2");
            expect_eq(row.at(2).GetInt64(), static_cast<std::int64_t>(42), "def2");
            // Insert must use new width.
            EXPECT_OK(txn->Insert("users",
                                  reldb::Row({reldb::Value::Int64(3),
                                              reldb::Value::String("c"),
                                              reldb::Value::Int64(7)})),
                      "i3");
            EXPECT_OK(txn->Commit(), "c2");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_alter_add_column_skips_deleted) {
    auto dir = MakeTempDir("reldb_alter_del");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("a")})),
                      "i1");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(2),
                                                       reldb::Value::String("b")})),
                      "i2");
            EXPECT_OK(txn->Commit(), "c1");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b2");
            EXPECT_OK(txn->Delete("users", reldb::Value::Int64(2)), "del");
            EXPECT_OK(txn->Commit(), "c2");
        }

        reldb::ColumnDef age{"age", reldb::ColumnType::kInt64, false};
        EXPECT_OK(db->AlterTableAddColumn("users", age, reldb::Value::Int64(0)), "alter");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b3");
            reldb::Row row;
            EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &row), "live");
            expect_eq(row.size(), static_cast<std::size_t>(3), "w");
            expect(txn->Get("users", reldb::Value::Int64(2), &row).IsNotFound(), "still del");
            EXPECT_OK(txn->Commit(), "c3");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_alter_add_column_rejects) {
    auto dir = MakeTempDir("reldb_alter_rej");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        reldb::ColumnDef age{"age", reldb::ColumnType::kInt64, false};
        expect(db->AlterTableAddColumn("nope", age, reldb::Value::Int64(0)).IsNotFound(),
               "missing table");

        reldb::ColumnDef pk{"x", reldb::ColumnType::kInt64, true};
        expect(db->AlterTableAddColumn("users", pk, reldb::Value::Int64(0))
                   .IsInvalidArgument(),
               "pk col");

        expect(db->AlterTableAddColumn("users", age, reldb::Value::Null()).IsInvalidArgument(),
               "null default");

        expect(db->AlterTableAddColumn("users", age, reldb::Value::String("x"))
                   .IsInvalidArgument(),
               "type mismatch");

        reldb::ColumnDef name_dup{"name", reldb::ColumnType::kString, false};
        expect(db->AlterTableAddColumn("users", name_dup, reldb::Value::String("z"))
                   .IsInvalidArgument(),
               "dup name");

        // Gate: open txn blocks ALTER.
        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        expect(db->AlterTableAddColumn("users", age, reldb::Value::Int64(1))
                   .IsInvalidArgument(),
               "blocked");
        EXPECT_OK(txn->Commit(), "commit");

        EXPECT_OK(db->AlterTableAddColumn("users", age, reldb::Value::Int64(1)), "ok after");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_alter_add_column_persists) {
    auto dir = MakeTempDir("reldb_alter_persist");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("a")})),
                      "i");
            EXPECT_OK(txn->Commit(), "c");
        }
        reldb::ColumnDef age{"age", reldb::ColumnType::kInt64, false};
        EXPECT_OK(db->AlterTableAddColumn("users", age, reldb::Value::Int64(9)), "alter");
    }
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "reopen");
        reldb::TableSchema got;
        EXPECT_OK(db->GetTable("users", &got), "schema");
        expect_eq(got.num_columns(), static_cast<std::size_t>(3), "ncols");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "b");
        reldb::Row row;
        EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &row), "get");
        expect_eq(row.at(2).GetInt64(), static_cast<std::int64_t>(9), "def");
        EXPECT_OK(txn->Commit(), "c");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_alter_add_column_string_default) {
    auto dir = MakeTempDir("reldb_alter_str");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("a")})),
                      "i");
            EXPECT_OK(txn->Commit(), "c");
        }
        reldb::ColumnDef city{"city", reldb::ColumnType::kString, false};
        EXPECT_OK(db->AlterTableAddColumn("users", city, reldb::Value::String("NYC")),
                  "alter");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "b2");
        reldb::Row row;
        EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &row), "get");
        expect_eq(row.at(2).GetString(), std::string("NYC"), "city");
        EXPECT_OK(txn->Commit(), "c2");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_alter_drop_column_empty_table) {
    auto dir = MakeTempDir("reldb_alter_drop_empty");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        EXPECT_OK(db->AlterTableDropColumn("users", "name"), "drop name");

        reldb::TableSchema got;
        EXPECT_OK(db->GetTable("users", &got), "get");
        expect_eq(got.num_columns(), static_cast<std::size_t>(1), "ncols");
        expect_eq(got.columns()[0].name, std::string("id"), "pk remains");
        expect(got.columns()[0].primary_key, "still pk");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_alter_drop_column_rewrites_rows) {
    auto dir = MakeTempDir("reldb_alter_drop_data");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        // Three columns so we can drop a middle non-PK.
        reldb::TableSchema wide("users", {
            {"id", reldb::ColumnType::kInt64, true},
            {"name", reldb::ColumnType::kString, false},
            {"age", reldb::ColumnType::kInt64, false},
        });
        EXPECT_OK(db->CreateTable(wide), "create");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            EXPECT_OK(txn->Insert("users",
                                  reldb::Row({reldb::Value::Int64(1),
                                              reldb::Value::String("a"),
                                              reldb::Value::Int64(30)})),
                      "i1");
            EXPECT_OK(txn->Insert("users",
                                  reldb::Row({reldb::Value::Int64(2),
                                              reldb::Value::String("b"),
                                              reldb::Value::Int64(40)})),
                      "i2");
            EXPECT_OK(txn->Commit(), "c");
        }

        EXPECT_OK(db->AlterTableDropColumn("users", "name"), "drop name");

        reldb::TableSchema got;
        EXPECT_OK(db->GetTable("users", &got), "schema");
        expect_eq(got.num_columns(), static_cast<std::size_t>(2), "ncols");
        expect_eq(got.columns()[0].name, std::string("id"), "id");
        expect_eq(got.columns()[1].name, std::string("age"), "age");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin2");
            reldb::Row row;
            EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &row), "g1");
            expect_eq(row.size(), static_cast<std::size_t>(2), "w1");
            expect_eq(row.at(0).GetInt64(), static_cast<std::int64_t>(1), "pk");
            expect_eq(row.at(1).GetInt64(), static_cast<std::int64_t>(30), "age kept");
            EXPECT_OK(txn->Get("users", reldb::Value::Int64(2), &row), "g2");
            expect_eq(row.at(1).GetInt64(), static_cast<std::int64_t>(40), "age2");
            // Inserts use new width.
            EXPECT_OK(txn->Insert("users",
                                  reldb::Row({reldb::Value::Int64(3),
                                              reldb::Value::Int64(50)})),
                      "i3");
            EXPECT_OK(txn->Commit(), "c2");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_alter_drop_column_skips_deleted) {
    auto dir = MakeTempDir("reldb_alter_drop_del");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("a")})),
                      "i1");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(2),
                                                       reldb::Value::String("b")})),
                      "i2");
            EXPECT_OK(txn->Commit(), "c1");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b2");
            EXPECT_OK(txn->Delete("users", reldb::Value::Int64(2)), "del");
            EXPECT_OK(txn->Commit(), "c2");
        }

        EXPECT_OK(db->AlterTableDropColumn("users", "name"), "drop");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b3");
            reldb::Row row;
            EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &row), "live");
            expect_eq(row.size(), static_cast<std::size_t>(1), "pk only");
            expect(txn->Get("users", reldb::Value::Int64(2), &row).IsNotFound(), "still del");
            EXPECT_OK(txn->Commit(), "c3");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_alter_drop_column_rejects) {
    auto dir = MakeTempDir("reldb_alter_drop_rej");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        expect(db->AlterTableDropColumn("nope", "name").IsNotFound(), "missing table");
        expect(db->AlterTableDropColumn("users", "").IsInvalidArgument(), "empty name");
        expect(db->AlterTableDropColumn("users", "missing").IsInvalidArgument(),
               "unknown col");
        expect(db->AlterTableDropColumn("users", "id").IsInvalidArgument(), "drop pk");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        expect(db->AlterTableDropColumn("users", "name").IsInvalidArgument(), "blocked");
        EXPECT_OK(txn->Commit(), "commit");

        EXPECT_OK(db->AlterTableDropColumn("users", "name"), "ok after");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_alter_drop_column_persists) {
    auto dir = MakeTempDir("reldb_alter_drop_persist");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("a")})),
                      "i");
            EXPECT_OK(txn->Commit(), "c");
        }
        EXPECT_OK(db->AlterTableDropColumn("users", "name"), "drop");
    }
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "reopen");
        reldb::TableSchema got;
        EXPECT_OK(db->GetTable("users", &got), "schema");
        expect_eq(got.num_columns(), static_cast<std::size_t>(1), "ncols");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "b");
        reldb::Row row;
        EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &row), "get");
        expect_eq(row.size(), static_cast<std::size_t>(1), "width");
        expect_eq(row.at(0).GetInt64(), static_cast<std::int64_t>(1), "pk");
        EXPECT_OK(txn->Commit(), "c");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_alter_add_then_drop) {
    auto dir = MakeTempDir("reldb_alter_add_drop");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "b");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("a")})),
                      "i");
            EXPECT_OK(txn->Commit(), "c");
        }
        reldb::ColumnDef age{"age", reldb::ColumnType::kInt64, false};
        EXPECT_OK(db->AlterTableAddColumn("users", age, reldb::Value::Int64(7)), "add");
        EXPECT_OK(db->AlterTableDropColumn("users", "age"), "drop age");

        reldb::TableSchema got;
        EXPECT_OK(db->GetTable("users", &got), "schema");
        expect_eq(got.num_columns(), static_cast<std::size_t>(2), "back to 2");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "b2");
        reldb::Row row;
        EXPECT_OK(txn->Get("users", reldb::Value::Int64(1), &row), "get");
        expect_eq(row.size(), static_cast<std::size_t>(2), "width");
        expect_eq(row.at(1).GetString(), std::string("a"), "name kept");
        EXPECT_OK(txn->Commit(), "c2");
    }
    RemoveDirRecursive(dir);
}
