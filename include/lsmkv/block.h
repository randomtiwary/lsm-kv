#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lsmkv/slice.h"
#include "lsmkv/status.h"

namespace lsmkv {

// Sorted key/value block format (LevelDB-style)
// =============================================
//
// A Block is the reusable on-disk unit used for both SSTable data blocks and
// the SSTable index block. Keys within a block are stored in sorted order
// (internal-key order when used by SSTables).
//
// Layout of a finished block:
//
//   +---------------------------+----------------------+------------------+
//   | entry[0] ... entry[N-1]   | restart_offset[0..R) | num_restarts     |
//   | (prefix-compressed)       | (fixed32 each, LE)   | (fixed32, LE)    |
//   +---------------------------+----------------------+------------------+
//
// Entry encoding (prefix compression against the previous key):
//
//   shared_bytes     : varint32   // bytes in common with previous key
//   non_shared_bytes : varint32   // bytes of key that differ
//   value_length     : varint32
//   key_delta        : non_shared_bytes raw bytes
//   value            : value_length raw bytes
//
// Full key = previous_key[0, shared) || key_delta.
//
// Restart points:
//   Every `restart_interval` entries (default 16), shared_bytes is forced to 0
//   and the absolute offset of that entry in the block is appended to the
//   restart array. Restart keys are stored in full so Seek can binary-search
//   the restart array, jump to a candidate entry, then scan linearly.
//   restart_offset[i] is a fixed32 byte offset from the start of the block.
//   num_restarts is the count of restart offsets; the restart array starts at
//   block_size - 4 - num_restarts * 4.
//
// BlockBuilder::Add appends entries; Finish() appends the restart array and
// trailer. Block::Iterator decodes entries and supports SeekToFirst / Seek /
// Next over the sorted sequence.

class BlockBuilder {
public:
    // restart_interval: emit a full-key restart every N entries (N >= 1).
    explicit BlockBuilder(int restart_interval = 16);

    void Reset();
    // Append one entry. Keys must be added in non-decreasing sort order.
    void Add(const Slice& key, const Slice& value);
    // Append restart array + count; returns a Slice into the internal buffer
    // valid until the next mutating call.
    Slice Finish();
    std::size_t CurrentSizeEstimate() const;
    bool empty() const { return buffer_.empty(); }

private:
    int restart_interval_;
    std::string buffer_;
    std::vector<std::uint32_t> restarts_;
    std::string last_key_;
    int counter_ = 0;
    bool finished_ = false;
};

class Block {
public:
    // Parse a finished block image (entries || restarts || num_restarts).
    explicit Block(std::string data);
    Status status() const { return status_; }

    class Iterator {
    public:
        explicit Iterator(const Block* block);
        bool Valid() const { return valid_; }
        Slice key() const { return key_; }
        Slice value() const { return value_; }
        void Next();
        void SeekToFirst();
        // Position at the first entry with key >= target (internal-key order).
        void Seek(const Slice& target);
        Status status() const { return status_; }

    private:
        const Block* block_;
        std::uint32_t restart_index_ = 0;
        const char* p_ = nullptr;
        const char* limit_ = nullptr;
        std::string key_buf_;
        Slice key_;
        Slice value_;
        bool valid_ = false;
        Status status_;

        void ParseNext();
        std::uint32_t NumRestarts() const;
        const char* RestartPoint(std::uint32_t index) const;
    };

    Iterator NewIterator() const { return Iterator(this); }
    std::size_t size() const { return data_.size(); }

private:
    std::string data_;
    std::uint32_t restart_offset_ = 0;
    Status status_;
    friend class Iterator;
};

}  // namespace lsmkv
