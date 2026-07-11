#pragma once

#include <cstdint>

namespace lsmkv {

// Logical time for snapshots, sequences, and higher-level MVCC clocks.
using Timestamp = std::uint64_t;

}  // namespace lsmkv
