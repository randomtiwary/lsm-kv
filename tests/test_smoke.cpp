#include "test_harness.h"

TEST(smoke_harness_works) {
    expect(true, "true is true");
    expect_eq(1, 1, "one equals one");
    expect_eq(std::string("lsm"), std::string("lsm"), "strings match");
}
