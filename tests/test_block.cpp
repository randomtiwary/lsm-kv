
#include "test_harness.h"
#include "lsmkv/block.h"

TEST(block_build_iterate_seek) {
    lsmkv::BlockBuilder b(2);
    b.Add("a", "1");
    b.Add("b", "2");
    b.Add("c", "3");
    b.Add("d", "4");
    auto slice = b.Finish();
    lsmkv::Block block(slice.ToString());
    expect(block.status().ok(), "status");
    auto it = block.NewIterator();
    it.SeekToFirst();
    expect(it.Valid(), "valid");
    expect_eq(it.key().ToString(), std::string("a"), "a");
    it.Next();
    expect_eq(it.key().ToString(), std::string("b"), "b");
    it.Seek("c");
    expect_eq(it.key().ToString(), std::string("c"), "seek c");
    expect_eq(it.value().ToString(), std::string("3"), "val3");
    it.Seek("z");
    expect(!it.Valid(), "past end");
}
