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

    bool ok() const { return code_ == kOk; }
    bool IsNotFound() const { return code_ == kNotFound; }
    bool IsCorruption() const { return code_ == kCorruption; }
    bool IsNotSupported() const { return code_ == kNotSupported; }
    bool IsInvalidArgument() const { return code_ == kInvalidArgument; }
    bool IsIOError() const { return code_ == kIOError; }

    std::string ToString() const;

private:
    enum Code {
        kOk = 0,
        kNotFound = 1,
        kCorruption = 2,
        kNotSupported = 3,
        kInvalidArgument = 4,
        kIOError = 5
    };

    Status(Code code, const std::string& msg) : code_(code), msg_(msg) {}

    Code code_;
    std::string msg_;
};

}  // namespace lsmkv
