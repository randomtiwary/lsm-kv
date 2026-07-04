#pragma once

#include <atomic>
#include <optional>
#include <memory>
#include <string>

#include "lsmkv/encoding.h"
#include "lsmkv/internal_key.h"
#include "lsmkv/skiplist.h"
#include "lsmkv/status.h"

namespace lsmkv {

// MemTable entries are: varint(ikey_len) + ikey + varint(vlen) + value
// Comparison uses only the internal key portion.
struct MemTableEntryComparator {
    int operator()(const std::string& a, const std::string& b) const {
        Slice sa(a), sb(b);
        Slice ka, kb;
        if (!GetLengthPrefixedSlice(&sa, &ka) || !GetLengthPrefixedSlice(&sb, &kb)) {
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        return CompareInternalKey(ka, kb);
    }
};

// In-memory write buffer backed by a concurrent SkipList.
//
// Deletes are not removed in place. Call Add(..., kTypeDeletion, key, /*value=*/"")
// to append a tombstone internal key. Get() reports outcomes through *found / *value
// and reserves Status for hard errors (e.g. corruption):
//   *found == false: key absent from this table (search older layers)
//   *found == true && !*value: tombstone; stop searching older layers
//   *found == true && *value: live value (may be an empty string)
// Older snapshots from before the tombstone sequence still see the prior value.
class MemTable {
public:
    MemTable();
    ~MemTable() = default;

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    // Insert a put (kTypeValue) or delete tombstone (kTypeDeletion). For deletions,
    // pass an empty value; only the encoded type tag is meaningful.
    void Add(std::uint64_t seq, ValueType type, const Slice& key, const Slice& value);

    // Lookup user_key visible at snapshot. `value` and `found` must be non-null
    // (enforced with LSMKV_DCHECK; enabled only for the CMake Debug config).
    // On success returns OK and:
    //   *found == false: miss in this table; *value is set to std::nullopt
    //   *found == true && *value == std::nullopt: definitive tombstone
    //   *found == true && *value has_value(): live value (possibly empty)
    Status Get(const Slice& user_key, std::uint64_t snapshot, std::optional<std::string>* value,
               bool* found) const;

    std::size_t ApproximateMemoryUsage() const;
    std::size_t EntryCount() const;

    using Table = SkipList<std::string, MemTableEntryComparator>;
    const Table& table() const { return *table_; }

    template <typename Fn>
    void ForEach(Fn fn) const {
        table_->Iterate([&](const std::string& encoded) {
            Slice s(encoded);
            Slice ikey;
            if (!GetLengthPrefixedSlice(&s, &ikey)) return;
            Slice val;
            if (!GetLengthPrefixedSlice(&s, &val)) return;
            fn(ikey, val);
        });
    }

private:
    std::unique_ptr<Table> table_;
    std::atomic<std::size_t> memory_usage_{0};
};

}  // namespace lsmkv
