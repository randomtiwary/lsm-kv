#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lsmkv/options.h"
#include "lsmkv/status.h"
#include "lsmkv/version.h"

namespace lsmkv {

struct CompactionResult {
    FileMetaData output;
    std::vector<std::pair<int, std::uint64_t>> inputs;  // level, number
};

// Merge all L0 files plus overlapping L1 files into a single L1 file.
Status CompactLevel0(const Options& options, const std::string& dbname, Version* current,
                     std::uint64_t output_number, CompactionResult* result);

}  // namespace lsmkv
