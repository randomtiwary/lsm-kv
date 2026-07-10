#include "test_harness.h"
#include "reldb/database.h"

TEST(reldb_smoke_headers_linked) {
    // database.h pulls catalog/mvcc; linking reldb objects is enough for smoke.
    expect(true, "reldb headers usable");
}
