
#include "test_harness.hpp"
#include "lsmkv/coding.hpp"

TEST(coding_fixed) {
    std::string buf;
    lsmkv::PutFixed32(&buf, 0x01020304u);
    lsmkv::PutFixed64(&buf, 0x0102030405060708ull);
    lsmkv::Slice s(buf);
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    expect(lsmkv::GetFixed32(&s, &u32), "g32");
    expect(lsmkv::GetFixed64(&s, &u64), "g64");
    expect_eq(static_cast<std::uint64_t>(u32), 0x01020304ull, "v32");
    expect_eq(u64, 0x0102030405060708ull, "v64");
}

TEST(coding_varint_and_length_prefixed) {
    std::string buf;
    lsmkv::PutVarint32(&buf, 300);
    lsmkv::PutVarint64(&buf, 0x100000000ull);
    lsmkv::PutLengthPrefixedSlice(&buf, "hi");
    lsmkv::Slice s(buf);
    std::uint32_t v32 = 0;
    std::uint64_t v64 = 0;
    lsmkv::Slice got;
    expect(lsmkv::GetVarint32(&s, &v32), "gv32");
    expect_eq(static_cast<std::uint64_t>(v32), 300ull, "300");
    expect(lsmkv::GetVarint64(&s, &v64), "gv64");
    expect_eq(v64, 0x100000000ull, "v64");
    expect(lsmkv::GetLengthPrefixedSlice(&s, &got), "glp");
    expect_eq(got.ToString(), std::string("hi"), "hi");
    expect_eq(static_cast<std::uint64_t>(lsmkv::VarintLength(300)), 2ull, "varlen");
}
