#include "lsmkv/memtable.h"

#include "lsmkv/debug.h"

namespace lsmkv {

MemTable::MemTable() : table_(new Table(MemTableEntryComparator())) {}

void MemTable::Add(Timestamp seq, ValueType type, const Slice& key, const Slice& value) {
    std::string ikey = MakeInternalKey(key, seq, type);
    std::string entry;
    PutLengthPrefixedSlice(&entry, ikey);
    PutLengthPrefixedSlice(&entry, value);
    memory_usage_.fetch_add(entry.size(), std::memory_order_relaxed);
    table_->Insert(entry);
}

Status MemTable::Get(const Slice& user_key, Timestamp snapshot,
                     std::optional<std::string>* value, bool* found) const {
    LSMKV_DCHECK(value != nullptr);
    LSMKV_DCHECK(found != nullptr);
    *found = false;
    *value = std::nullopt;
    std::string lkey = MakeLookupKey(user_key, snapshot);
    std::string seek_entry;
    PutLengthPrefixedSlice(&seek_entry, lkey);
    PutLengthPrefixedSlice(&seek_entry, Slice());

    std::string entry;
    if (!table_->Find(seek_entry, &entry)) {
        return STATUS(OK);
    }
    Slice s(entry);
    Slice ikey;
    if (!GetLengthPrefixedSlice(&s, &ikey)) return STATUS(Corruption, "bad memtable entry");
    if (ExtractUserKey(ikey).compare(user_key) != 0) return STATUS(OK);
    if (ExtractSequence(ikey) > snapshot) return STATUS(OK);
    *found = true;
    if (ExtractValueType(ikey) == kTypeDeletion) {
        *value = std::nullopt;  // definitive tombstone
        return STATUS(OK);
    }
    Slice val;
    if (!GetLengthPrefixedSlice(&s, &val)) return STATUS(Corruption, "bad memtable value");
    value->emplace(val.data(), val.size());
    return STATUS(OK);
}

std::size_t MemTable::ApproximateMemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
}

std::size_t MemTable::EntryCount() const { return table_->Size(); }

}  // namespace lsmkv
