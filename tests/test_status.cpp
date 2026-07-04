#include "test_harness.h"
#include "lsmkv/status.h"

TEST(status_ok) {
    auto s = STATUS(OK);
    expect(s.ok(), "ok");
    expect_eq(s.ToString(), std::string("OK"), "to string");
}

TEST(status_not_found) {
    auto s = STATUS(NotFound, "missing");
    expect(s.IsNotFound(), "is not found");
    expect(!s.ok(), "not ok");
    expect(s.ToString().find("missing") != std::string::npos, "msg");
}

TEST(status_corruption_io) {
    expect(STATUS(Corruption, "x").IsCorruption(), "corr");
    expect(STATUS(IOError, "y").IsIOError(), "io");
    expect(STATUS(InvalidArgument, "z").IsInvalidArgument(), "inv");
    expect(STATUS(NotSupported, "n").IsNotSupported(), "ns");
}

TEST(status_macro_default_msg) {
    auto s = STATUS(NotFound);
    expect(s.IsNotFound(), "default not found");
    expect_eq(s.ToString(), std::string("NotFound"), "no msg suffix");
}
