#pragma once

#include <cstddef>
#include <cstdint>

#include "lsmkv/common.h"

namespace lsmkv {

struct Options {
    bool create_if_missing = false;
    bool error_if_exists = false;

    // Approximate MemTable size that triggers a flush.
    std::size_t write_buffer_size = 4 * 1024 * 1024;  // 4 MiB

    // Target SSTable data block size.
    std::size_t block_size = 4 * 1024;

    // Restart interval inside a block.
    int block_restart_interval = 16;

    // Flush L0 -> L1 when this many L0 files exist.
    int level0_compaction_trigger = 4;

    // Max levels (we use 0 and 1 only in v1, but reserve room).
    int num_levels = 2;
};

struct ReadOptions {
    // Reserved for future snapshot support.
    Timestamp snapshot = 0;
};

struct WriteOptions {
    // If true, flush WAL to disk before returning from Put/Delete.
    bool sync = false;
};

}  // namespace lsmkv
