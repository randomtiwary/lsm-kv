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

reldb::TableSchema AccountsSchema() {
    return reldb::TableSchema("accounts", {
        {"uid", reldb::ColumnType::kString, true},
        {"bal", reldb::ColumnType::kInt64, false},
    });
}

// Prefix-related sibling: "user" must not be confused with "users".
reldb::TableSchema UserSchema() {
    return reldb::TableSchema("user", {
        {"id", reldb::ColumnType::kInt64, true},
    });
}

std::shared_ptr<reldb::Database> OpenDb(const std::string& dir) {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::shared_ptr<reldb::Database> db;
    if (!reldb::Database::Open(opt, dir, &db).ok()) return nullptr;
    return db;
}

// Count live user keys under a prefix (tombstones hidden by the iterator).
std::size_t CountPrefix(lsmkv::DB* kv, const std::string& prefix) {
    std::size_t n = 0;
    auto it = kv->NewIterator(lsmkv::ReadOptions());
    it->Seek(prefix);
    while (it->Valid() && it->key().starts_with(prefix)) {
        ++n;
        it->Next();
    }
    return n;
}

}  // namespace

TEST(reldb_drop_table_empty) {
    auto dir = MakeTempDir("reldb_drop_empty");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        EXPECT_OK(db->DropTable("users"), "drop");
        reldb::TableSchema got;
        expect(db->GetTable("users", &got).IsNotFound(), "gone");
        bool exists = true;
        EXPECT_OK(db->HasTable("users", &exists), "has");
        expect(!exists, "has false");

        // Recreate is allowed.
        EXPECT_OK(db->CreateTable(UsersSchema()), "recreate");
        EXPECT_OK(db->GetTable("users", &got), "get again");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_drop_table_with_data) {
    auto dir = MakeTempDir("reldb_drop_data");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        // Insert, update, delete to leave heads + version history.
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("a")})),
                      "ins1");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(2),
                                                       reldb::Value::String("b")})),
                      "ins2");
            EXPECT_OK(txn->Commit(), "c1");
        }
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin2");
            EXPECT_OK(txn->Update("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("a2")})),
                      "upd");
            EXPECT_OK(txn->Delete("users", reldb::Value::Int64(2)), "del");
            EXPECT_OK(txn->Commit(), "c2");
        }

        expect(CountPrefix(db->kv().get(), "d/users/") > 0, "heads present");
        expect(CountPrefix(db->kv().get(), "v/users/") > 0, "versions present");

        EXPECT_OK(db->DropTable("users"), "drop");

        reldb::TableSchema got;
        expect(db->GetTable("users", &got).IsNotFound(), "catalog gone");
        expect_eq(CountPrefix(db->kv().get(), "d/users/"), static_cast<std::size_t>(0),
                  "heads gc");
        expect_eq(CountPrefix(db->kv().get(), "v/users/"), static_cast<std::size_t>(0),
                  "versions gc");
        // Catalog key removed.
        expect_eq(CountPrefix(db->kv().get(), "c/t/users"), static_cast<std::size_t>(0),
                  "catalog key");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_drop_table_missing_and_gate) {
    auto dir = MakeTempDir("reldb_drop_gate");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");

        expect(db->DropTable("nope").IsNotFound(), "missing");

        EXPECT_OK(db->CreateTable(UsersSchema()), "create");

        std::unique_ptr<reldb::Transaction> txn;
        EXPECT_OK(db->Begin(&txn), "begin");
        auto st = db->DropTable("users");
        expect(st.IsInvalidArgument(), "blocked");
        expect(st.ToString().find("DDL requires no open transactions") != std::string::npos,
               "msg");

        EXPECT_OK(txn->Commit(), "commit");

        db->TEST_SetDdlInProgress(true);
        st = db->DropTable("users");
        expect(st.IsInvalidArgument(), "ddl flag");
        expect(st.ToString().find("DDL in progress") != std::string::npos, "ddl msg");
        db->TEST_SetDdlInProgress(false);

        EXPECT_OK(db->DropTable("users"), "drop after gate clear");
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_drop_table_isolates_siblings) {
    auto dir = MakeTempDir("reldb_drop_sib");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "users");
        EXPECT_OK(db->CreateTable(AccountsSchema()), "accounts");
        EXPECT_OK(db->CreateTable(UserSchema()), "user");

        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("u")})),
                      "iu");
            EXPECT_OK(txn->Insert("accounts",
                                  reldb::Row({reldb::Value::String("x"),
                                              reldb::Value::Int64(10)})),
                      "ia");
            EXPECT_OK(txn->Insert("user", reldb::Row({reldb::Value::Int64(9)})), "iuser");
            EXPECT_OK(txn->Commit(), "c");
        }

        EXPECT_OK(db->DropTable("users"), "drop users");

        reldb::TableSchema got;
        expect(db->GetTable("users", &got).IsNotFound(), "users gone");
        EXPECT_OK(db->GetTable("accounts", &got), "accounts stay");
        EXPECT_OK(db->GetTable("user", &got), "user stay");

        expect_eq(CountPrefix(db->kv().get(), "d/users/"), static_cast<std::size_t>(0),
                  "users heads");
        expect(CountPrefix(db->kv().get(), "d/accounts/") > 0, "accounts heads");
        expect(CountPrefix(db->kv().get(), "d/user/") > 0, "user heads");

        // Row still readable on surviving tables.
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin2");
            reldb::Row row;
            EXPECT_OK(txn->Get("accounts", reldb::Value::String("x"), &row), "get acc");
            EXPECT_OK(txn->Get("user", reldb::Value::Int64(9), &row), "get user");
            EXPECT_OK(txn->Commit(), "c2");
        }
    }
    RemoveDirRecursive(dir);
}

TEST(reldb_drop_table_persists) {
    auto dir = MakeTempDir("reldb_drop_persist");
    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "open");
        EXPECT_OK(db->CreateTable(UsersSchema()), "create");
        {
            std::unique_ptr<reldb::Transaction> txn;
            EXPECT_OK(db->Begin(&txn), "begin");
            EXPECT_OK(txn->Insert("users", reldb::Row({reldb::Value::Int64(1),
                                                       reldb::Value::String("a")})),
                      "ins");
            EXPECT_OK(txn->Commit(), "c");
        }
        EXPECT_OK(db->DropTable("users"), "drop");
    }

    {
        auto db = OpenDb(dir);
        expect(db != nullptr, "reopen");
        reldb::TableSchema got;
        expect(db->GetTable("users", &got).IsNotFound(), "still gone");
        expect_eq(CountPrefix(db->kv().get(), "d/users/"), static_cast<std::size_t>(0),
                  "no heads");
        expect_eq(CountPrefix(db->kv().get(), "v/users/"), static_cast<std::size_t>(0),
                  "no vers");
    }
    RemoveDirRecursive(dir);
}
