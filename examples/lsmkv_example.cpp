// Minimal end-to-end demo of DB::Open / Put / Get and reopen.
//
// Build & run from the repo root with:
//   ./scripts/run_example.sh
//
// Or manually:
//   cmake -S . -B build -DLSMKV_BUILD_EXAMPLES=ON
//   cmake --build build --target lsmkv_example
//   ./build/lsmkv_example
//
// The program writes to /tmp/lsmkv_example_db (deleted at start).

#include <iostream>
#include <cstdlib>
#include <string>

#include "lsmkv/db.h"

int main() {
    const std::string dbpath = "/tmp/lsmkv_example_db";
    std::system(("rm -rf " + dbpath).c_str());

    lsmkv::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 64 * 1024;

    lsmkv::DB* db = nullptr;
    lsmkv::Status s = lsmkv::DB::Open(options, dbpath, &db);
    if (!s.ok()) {
        std::cerr << "open failed: " << s.ToString() << "\n";
        return 1;
    }

    s = db->Put(lsmkv::WriteOptions(), "hello", "world");
    if (!s.ok()) {
        std::cerr << "put failed: " << s.ToString() << "\n";
        return 1;
    }

    std::string value;
    s = db->Get(lsmkv::ReadOptions(), "hello", &value);
    if (!s.ok()) {
        std::cerr << "get failed: " << s.ToString() << "\n";
        return 1;
    }
    std::cout << "hello => " << value << "\n";

    delete db;

    s = lsmkv::DB::Open(options, dbpath, &db);
    if (!s.ok()) {
        std::cerr << "reopen failed: " << s.ToString() << "\n";
        return 1;
    }
    value.clear();
    s = db->Get(lsmkv::ReadOptions(), "hello", &value);
    if (!s.ok()) {
        std::cerr << "get after reopen failed: " << s.ToString() << "\n";
        return 1;
    }
    std::cout << "after reopen: hello => " << value << "\n";
    delete db;
    std::cout << "example ok\n";
    return 0;
}
