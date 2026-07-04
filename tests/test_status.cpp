
#include "test_harness.hpp"
#include "lsmkv/status.hpp"

TEST(status_ok) {
    auto s = lsmkv::Status::OK();
    expect(s.ok(), "ok");
    expect_eq(s.ToString(), std::string("OK"), "to string");
}

TEST(status_not_found) {
    auto s = lsmkv::Status::NotFound("missing");
    expect(s.IsNotFound(), "is not found");
    expect(!s.ok(), "not ok");
    expect(s.ToString().find("missing") != std::string::npos, "msg");
}

TEST(status_corruption_io) {
    expect(lsmkv::Status::Corruption("x").IsCorruption(), "corr");
    expect(lsmkv::Status::IOError("y").IsIOError(), "io");
    expect(lsmkv::Status::InvalidArgument("z").IsInvalidArgument(), "inv");
    expect(lsmkv::Status::NotSupported("n").IsNotSupported(), "ns");
}
