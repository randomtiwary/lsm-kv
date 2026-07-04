#pragma once

#include <string>

#include "lsmkv/options.h"
#include "lsmkv/slice.h"
#include "lsmkv/status.h"

namespace lsmkv {

class DB {
public:
    static Status Open(const Options& options, const std::string& name, DB** dbptr);

    DB() = default;
    virtual ~DB();

    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

    virtual Status Put(const WriteOptions& options, const Slice& key, const Slice& value) = 0;
    virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;
    virtual Status Get(const ReadOptions& options, const Slice& key, std::string* value) = 0;
};

}  // namespace lsmkv
