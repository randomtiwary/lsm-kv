
#include "test_harness.h"
#include "lsmkv/slice.h"

TEST(slice_basic) {
    lsmkv::Slice s("hello");
    expect_eq(s.size(), std::size_t{5}, "size");
    expect(!s.empty(), "not empty");
    expect_eq(s.ToString(), std::string("hello"), "to string");
    expect(s.starts_with("he"), "prefix");
}

TEST(slice_compare) {
    lsmkv::Slice a("abc");
    lsmkv::Slice b("abd");
    expect(a < b, "less");
    expect(a == lsmkv::Slice("abc"), "eq");
    expect(a != b, "neq");
}

TEST(slice_remove_prefix) {
    std::string backing = "abcdef";
    lsmkv::Slice s(backing);
    s.remove_prefix(2);
    expect_eq(s.ToString(), std::string("cdef"), "removed");
}
