#pragma once

// Small RAII helpers shared across reldb.

namespace reldb {

// On destruction, assigns restore_value to the referenced flag.
// Typical use: set a flag to true, then ScopedBool restores false on scope exit.
//
//   flag = true;
//   ScopedBool restore(flag, false);
//   // ... work ...
//   // dtor: flag = false
class ScopedBool {
public:
    ScopedBool(bool& flag, bool restore_value) : flag_(flag), restore_value_(restore_value) {}

    ~ScopedBool() { flag_ = restore_value_; }

    ScopedBool(const ScopedBool&) = delete;
    ScopedBool& operator=(const ScopedBool&) = delete;

private:
    bool& flag_;
    bool restore_value_;
};

}  // namespace reldb
