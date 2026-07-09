#pragma once

#include <string>

namespace lsmkv {

class Status {
public:
    Status() : code_(kOk) {}

    static Status OK() { return Status(); }
    static Status NotFound(const std::string& msg = "") { return Status(kNotFound, msg); }
    static Status Corruption(const std::string& msg) { return Status(kCorruption, msg); }
    static Status NotSupported(const std::string& msg) { return Status(kNotSupported, msg); }
    static Status InvalidArgument(const std::string& msg) { return Status(kInvalidArgument, msg); }
    static Status IOError(const std::string& msg) { return Status(kIOError, msg); }
    // Write-write conflict under snapshot isolation (relational layer).
    static Status Conflict(const std::string& msg) { return Status(kConflict, msg); }

    bool ok() const { return code_ == kOk; }
    bool IsNotFound() const { return code_ == kNotFound; }
    bool IsCorruption() const { return code_ == kCorruption; }
    bool IsNotSupported() const { return code_ == kNotSupported; }
    bool IsInvalidArgument() const { return code_ == kInvalidArgument; }
    bool IsIOError() const { return code_ == kIOError; }
    bool IsConflict() const { return code_ == kConflict; }

    std::string ToString() const;

private:
    enum Code {
        kOk = 0,
        kNotFound = 1,
        kCorruption = 2,
        kNotSupported = 3,
        kInvalidArgument = 4,
        kIOError = 5,
        kConflict = 6
    };

    Status(Code code, const std::string& msg) : code_(code), msg_(msg) {}

    Code code_;
    std::string msg_;
};

}  // namespace lsmkv

// STATUS(NotFound, "missing key") -> lsmkv::Status::NotFound("missing key")
// STATUS(OK) -> lsmkv::Status::OK()
// Implemented without empty __VA_ARGS__ so -Wpedantic stays clean.
#define STATUS(...) LSMKV_STATUS_HELPER(__VA_ARGS__, 2, 1, ~)
#define LSMKV_STATUS_HELPER(a, b, n, ...) LSMKV_STATUS_##n(a, b)
#define LSMKV_STATUS_1(Code, _) (::lsmkv::Status::Code())
#define LSMKV_STATUS_2(Code, msg) (::lsmkv::Status::Code(msg))
