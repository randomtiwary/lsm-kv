#pragma once

#include <memory>
#include <string>

#include "lsmkv/options.h"
#include "lsmkv/slice.h"
#include "lsmkv/status.h"

namespace lsmkv {

// Ordered scan over user keys visible at a read snapshot (same rules as Get).
// Caller owns the iterator via unique_ptr. Not safe to use after the DB is closed.
class Iterator {
public:
    Iterator() = default;
    virtual ~Iterator();

    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;

    virtual bool Valid() const = 0;
    // Position at the first user key in the DB (if any).
    virtual void SeekToFirst() = 0;
    // Position at the first user key >= target (if any).
    virtual void Seek(const Slice& target) = 0;
    virtual void Next() = 0;
    // Current user key / value. Only valid when Valid() is true.
    virtual Slice key() const = 0;
    virtual Slice value() const = 0;
    virtual Status status() const = 0;
};

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

    // Snapshot is ReadOptions::snapshot if non-zero, else the last sequence at creation.
    // Yields one entry per user key that has a live value at that snapshot (tombstones hidden).
    virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;
};

}  // namespace lsmkv
