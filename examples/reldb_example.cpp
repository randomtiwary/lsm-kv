// Minimal walk-through of the relational MVCC API.
#include <iostream>

#include "lsmkv/options.h"
#include "reldb/database.h"
#include "reldb/row.h"
#include "reldb/schema.h"
#include "reldb/txn.h"
#include "reldb/types.h"

int main() {
    lsmkv::Options opt;
    opt.create_if_missing = true;
    reldb::Database* db = nullptr;
    auto st = reldb::Database::Open(opt, "/tmp/reldb_example", &db);
    if (!st.ok()) {
        std::cerr << "open: " << st.ToString() << "\n";
        return 1;
    }

    reldb::TableSchema schema("users", {
        {"id", reldb::ColumnType::kInt64, true},
        {"name", reldb::ColumnType::kString, false},
    });
    st = db->CreateTable(schema);
    if (!st.ok() && st.ToString().find("already exists") == std::string::npos) {
        std::cerr << "create: " << st.ToString() << "\n";
        delete db;
        return 1;
    }

    reldb::Transaction* txn = nullptr;
    db->Begin(&txn);
    reldb::Row row({reldb::Value::Int64(1), reldb::Value::String("ada")});
    st = txn->Insert("users", row);
    if (st.ok()) {
        st = txn->Commit();
        std::cout << "insert commit: " << st.ToString() << "\n";
    } else {
        std::cout << "insert (maybe exists): " << st.ToString() << "\n";
        txn->Abort();
    }
    delete txn;

    db->Begin(&txn);
    reldb::Row got;
    st = txn->Get("users", reldb::Value::Int64(1), &got);
    if (st.ok()) {
        std::cout << "user 1 name = " << got.at(1).GetString() << "\n";
    } else {
        std::cout << "get: " << st.ToString() << "\n";
    }
    txn->Abort();
    delete txn;
    delete db;
    return 0;
}
