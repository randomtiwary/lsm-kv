#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "lsmkv/block.h"
#include "lsmkv/options.h"
#include "lsmkv/status.h"

namespace lsmkv {

// SSTable (Sorted String Table) file format
// =========================================
//
// An SSTable is an immutable, sorted map from internal keys to values, stored
// as a sequence of data blocks plus one index block and a fixed footer.
//
// File layout:
//
//   +------------------+-----+------------------+-----+--------+
//   | data block 0     | crc | data block 1     | crc |  ...   |
//   +------------------+-----+------------------+-----+--------+
//   | index block      | crc | footer (20 bytes)               |
//   +------------------+-----+---------------------------------+
//
// Each block on disk is:  <Block contents> || fixed32(MaskCrc(Crc32(contents)))
// where <Block contents> uses the format documented in block.h. The CRC covers
// only the block bytes, not the trailer itself (same masking as the WAL).
//
// Data blocks hold user entries: key = internal key (user_key || pack(seq,type)),
// value = user value bytes (empty for deletions). Entries are added in sorted
// internal-key order. A data block is flushed when its estimated size reaches
// Options::block_size.
//
// Index block maps separator keys to data-block handles. After each data block
// is written, one index entry is added:
//   index key   = last internal key in that data block
//   index value = BlockHandle = fixed64(offset) || fixed64(size)
//                where offset/size refer to the block *contents* (excluding CRC)
// Index block uses restart_interval = 1 (no prefix compression) for simpler seeks.
//
// Footer (last 20 bytes, little-endian):
//   fixed64 index_block_offset
//   fixed64 index_block_size      // contents size, excluding CRC trailer
//   fixed32 magic = 0xDB04734E    // identifies the file as an lsm-kv SSTable
//
// Point lookup (SSTable::Get):
//   1. Read footer -> load index block (verify CRC).
//   2. Seek index for first entry with key >= target internal key.
//   3. Decode BlockHandle from index value; load that data block.
//   4. Seek inside the data block; compare user key / sequence / type.
//
// Iteration walks index entries in order, loading each data block on demand.
//
// FileMetaData records table identity and key bounds for the Version/MANIFEST
// layer (smallest/largest are encoded internal keys).

struct FileMetaData {
    std::uint64_t number = 0;
    std::uint64_t file_size = 0;
    std::string smallest;  // smallest internal key in the table
    std::string largest;   // largest internal key in the table
};

class MemTable;

class SSTableBuilder {
public:
    SSTableBuilder(const Options& options, std::string path);
    Status Open();
    // Append one internal-key / value pair. Keys must be in sorted order.
    void Add(const Slice& key, const Slice& value);
    // Flush any pending data block, write index block + footer, fill *meta.
    Status Finish(FileMetaData* meta);
    std::uint64_t FileSize() const { return offset_; }

private:
    // Serialize block contents, append masked CRC trailer, update file offset.
    Status WriteBlock(BlockBuilder* block, std::uint64_t* block_offset, std::uint64_t* block_size);
    Status FlushDataBlock();

    Options options_;
    std::string path_;
    std::ofstream out_;
    BlockBuilder data_block_;
    BlockBuilder index_block_;
    std::string last_key_;
    std::string pending_index_key_;
    std::uint64_t pending_offset_ = 0;
    std::uint64_t pending_size_ = 0;
    bool pending_index_entry_ = false;
    std::uint64_t offset_ = 0;
    bool open_ = false;
    std::string smallest_;
    std::string largest_;
    bool has_keys_ = false;
    Status status_;
};

class SSTable {
public:
    static Status Open(const std::string& path, std::unique_ptr<SSTable>* table);
    // Seek for internal_key; on a visible live value sets *value and *found=true.
    // Tombstone: *found=true and returns NotFound. Miss: *found=false, OK.
    Status Get(const Slice& internal_key, std::string* value, bool* found) const;

    class Iterator {
    public:
        explicit Iterator(const SSTable* table);
        ~Iterator();
        bool Valid() const;
        Slice key() const;
        Slice value() const;
        void Next();
        void Seek(const Slice& target);
        void SeekToFirst();
        Status status() const { return status_; }

    private:
        void InitDataBlock();

        const SSTable* table_;
        std::unique_ptr<Block> index_block_;
        std::unique_ptr<Block> data_block_;
        std::unique_ptr<Block::Iterator> index_iter_;
        std::unique_ptr<Block::Iterator> data_iter_;
        Status status_;
    };

    std::unique_ptr<Iterator> NewIterator() const;
    const std::string& path() const { return path_; }

private:
    explicit SSTable(std::string path);
    // Load block contents at [offset, offset+size) and verify trailing masked CRC.
    Status ReadBlock(std::uint64_t offset, std::uint64_t size, std::string* result) const;

    std::string path_;
    std::string file_data_;
    std::uint64_t index_offset_ = 0;
    std::uint64_t index_size_ = 0;
};

Status BuildTableFromMemTable(const Options& options, const std::string& path,
                              const MemTable& mem, std::uint64_t file_number,
                              FileMetaData* meta);

}  // namespace lsmkv
