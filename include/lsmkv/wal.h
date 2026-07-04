#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "lsmkv/internal_key.h"
#include "lsmkv/slice.h"
#include "lsmkv/status.h"

namespace lsmkv {

// Write-ahead log format
// ======================
//
// A WAL file is an append-only sequence of framed records. Each physical record
// on disk is:
//
//   +-------------------+----------------------+------------------+
//   | length (fixed32)  | crc32 (fixed32,      | payload (length  |
//   | little-endian     | masked, LE)          | bytes)           |
//   +-------------------+----------------------+------------------+
//        4 bytes               4 bytes              `length` bytes
//
// - `length` is the size of `payload` only (not including the 8-byte header).
// - `crc32` covers exactly the `payload` bytes. It is stored masked with
//   MaskCrc() (LevelDB-style) so embedded CRCs are unlikely to look like
//   valid headers if a reader loses sync.
// - `payload` is opaque to WalWriter/WalReader. For DB writes it is a
//   serialized write batch (see EncodeWriteBatch / DecodeWriteBatch below).
//
// WalReader::ReadRecord returns STATUS(NotFound) at clean EOF, and
// STATUS(Corruption) on a truncated header/payload or CRC mismatch.
//
// Write-batch payload (logical record contents)
// ---------------------------------------------
//
// EncodeWriteBatch serializes one atomic group of puts/deletes that share a
// starting sequence number. Layout (all multi-byte integers little-endian):
//
//   +------------------+-------------------+-----------------------------+
//   | start_seq        | count             | entry[0] ... entry[count-1] |
//   | (fixed64)        | (fixed32)         |                             |
//   +------------------+-------------------+-----------------------------+
//
// Each entry encodes one user operation as:
//
//   +------+---------------------------+-------------------------------+
//   | type | user_key                  | value                         |
//   | u8   | length-prefixed slice     | length-prefixed slice         |
//   +------+---------------------------+-------------------------------+
//
// - `type` is a ValueType byte: kTypeValue (0x1) for Put, kTypeDeletion (0x0)
//   for Delete. For deletions the value slice is present but empty/ignored.
// - A length-prefixed slice is: varint32(len) || len bytes of data
//   (see PutLengthPrefixedSlice / GetLengthPrefixedSlice in encoding.h).
// - `start_seq` is the sequence assigned to entry[0]; entry[i] uses
//   start_seq + i when replayed into the MemTable.
//
// Example: EncodeWriteBatch(9, {{kTypeValue, {"a","1"}},
//                               {kTypeDeletion, {"b",""}}}) produces:
//   fixed64(9) || fixed32(2)
//     || 0x01 || varint(1) || 'a' || varint(1) || '1'
//     || 0x00 || varint(1) || 'b' || varint(0)
//
// Round-trip: decode reads the header, then repeatedly pulls type + key +
// value until `count` entries are consumed. Trailing bytes are ignored;
// truncated fields yield STATUS(Corruption).

class WalWriter {
public:
    explicit WalWriter(std::string path);
    ~WalWriter();

    Status Open();
    // Append one framed record: fixed32(len) || fixed32(masked_crc) || data.
    Status AddRecord(const Slice& data);
    Status Sync();
    Status Close();
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::ofstream out_;
    bool open_ = false;
};

class WalReader {
public:
    explicit WalReader(std::string path);
    Status Open();
    // Read the next framed record into *record (payload only). See file comment
    // for header layout and status meanings (NotFound = EOF, Corruption = bad).
    Status ReadRecord(std::string* record);
    void Close();

private:
    std::string path_;
    std::ifstream in_;
    bool open_ = false;
};

// One batch entry: (ValueType, (user_key, value)).
using BatchEntry = std::pair<ValueType, std::pair<std::string, std::string>>;

// Serialize / parse the write-batch payload described above. These operate on
// the logical payload only; framing (length + CRC) is handled by WalWriter /
// WalReader.
std::string EncodeWriteBatch(std::uint64_t seq, const std::vector<BatchEntry>& entries);
Status DecodeWriteBatch(const Slice& record, std::uint64_t* seq, std::vector<BatchEntry>* entries);

}  // namespace lsmkv
