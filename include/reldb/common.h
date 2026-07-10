#pragma once

#include <cstdint>

namespace reldb {

// Logical MVCC / transaction time. Distinct from raw integer storage widths.
using Timestamp = std::uint64_t;

}  // namespace reldb
