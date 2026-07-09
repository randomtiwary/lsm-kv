#include "test_harness.h"
#include "reldb/database.h"

TEST(reldb_smoke_library_linked) {
    expect(reldb::LibraryName() != nullptr, "library name non-null");
    expect_eq(std::string(reldb::LibraryName()), std::string("reldb"), "library name");
}
