// DB user-key iterator
// ====================
//
// Goal: walk the database in user-key order and, for each user key, yield the
// same live value that Get() would return at a fixed sequence snapshot (or
// hide the key if the newest visible version is a tombstone).
//
// Layers in this file (bottom → top):
//
//   1. InternalIter
//      Abstract stream of *internal* keys (user_key + seq + type) + values.
//      Two implementations:
//        - MemTableInternalIter  — skiplist over mem_ / imm_ entries
//        - SSTableInternalIter   — one open SSTable::Iterator per file
//
//   2. MergingInternalIter
//      Merges many InternalIters by internal-key order (user key ascending,
//      sequence descending). At any step, Valid() points at the "smallest"
//      internal key among children — so for a given user key the highest
//      sequence appears first.
//
//   3. DBIterator (public lsmkv::Iterator)
//      On construction, snapshots mem_, imm_, Version, and the read sequence
//      (same as Get). Builds child iters for mem, imm, and every L0/L1 file.
//      AdvanceToNextVisibleKey():
//        - skip internal keys with seq > snapshot
//        - take the first remaining internal key for a user key (newest
//          visible version)
//        - skip all further versions of that user key
//        - if that version is a deletion, continue; else expose user key/value
//
// Seek(user_key) builds the same lookup internal key as Get, seeks the merge,
// then runs AdvanceToNextVisibleKey so the first emitted key is >= target.

#include "db_impl.h"

#include <algorithm>
#include <vector>

#include "lsmkv/encoding.h"
#include "lsmkv/env.h"
#include "lsmkv/internal_key.h"
#include "lsmkv/sstable.h"

namespace lsmkv {
namespace {

// Internal-key stream used to merge memtables and SSTables.
class InternalIter {
public:
    virtual ~InternalIter() = default;
    virtual bool Valid() const = 0;
    virtual void SeekToFirst() = 0;
    virtual void Seek(const Slice& target_ikey) = 0;
    virtual void Next() = 0;
    virtual Slice key() const = 0;    // internal key
    virtual Slice value() const = 0;
    virtual Status status() const = 0;
};

// MemTable skiplist entries are length-prefixed ikey + length-prefixed value.
class MemTableInternalIter : public InternalIter {
public:
    explicit MemTableInternalIter(const MemTable* mem)
        : it_(mem->table().NewIterator()) {}

    bool Valid() const override { return it_.Valid() && status_.ok(); }

    void SeekToFirst() override {
        it_.SeekToFirst();
        Parse();
    }

    void Seek(const Slice& target_ikey) override {
        std::string seek_entry;
        PutLengthPrefixedSlice(&seek_entry, target_ikey);
        PutLengthPrefixedSlice(&seek_entry, Slice());
        it_.Seek(seek_entry);
        Parse();
    }

    void Next() override {
        it_.Next();
        Parse();
    }

    Slice key() const override { return ikey_; }
    Slice value() const override { return value_; }
    Status status() const override { return status_; }

private:
    void Parse() {
        ikey_ = Slice();
        value_ = Slice();
        if (!it_.Valid()) return;
        Slice s(it_.key());
        Slice ikey;
        Slice val;
        if (!GetLengthPrefixedSlice(&s, &ikey) || !GetLengthPrefixedSlice(&s, &val)) {
            status_ = STATUS(Corruption, "bad memtable entry");
            return;
        }
        // Keep copies stable while the skiplist node lives under the iterator lock.
        ikey_storage_.assign(ikey.data(), ikey.size());
        value_storage_.assign(val.data(), val.size());
        ikey_ = Slice(ikey_storage_);
        value_ = Slice(value_storage_);
    }

    MemTable::Table::Iterator it_;
    Status status_;
    std::string ikey_storage_;
    std::string value_storage_;
    Slice ikey_;
    Slice value_;
};

class SSTableInternalIter : public InternalIter {
public:
    explicit SSTableInternalIter(std::unique_ptr<SSTable> table)
        : table_(std::move(table)), it_(table_->NewIterator()) {}

    bool Valid() const override { return it_->Valid() && status_.ok() && it_->status().ok(); }

    void SeekToFirst() override { it_->SeekToFirst(); }
    void Seek(const Slice& target_ikey) override { it_->Seek(target_ikey); }
    void Next() override { it_->Next(); }
    Slice key() const override { return it_->key(); }
    Slice value() const override { return it_->value(); }
    Status status() const override {
        if (!status_.ok()) return status_;
        return it_->status();
    }

private:
    std::unique_ptr<SSTable> table_;
    std::unique_ptr<SSTable::Iterator> it_;
    Status status_;
};

// Smallest-key-first merge over InternalIters (internal key order).
class MergingInternalIter {
public:
    void Add(std::unique_ptr<InternalIter> child) {
        if (child) children_.push_back(std::move(child));
    }

    void SeekToFirst() {
        for (auto& c : children_) c->SeekToFirst();
        FindSmallest();
    }

    void Seek(const Slice& target_ikey) {
        for (auto& c : children_) c->Seek(target_ikey);
        FindSmallest();
    }

    void Next() {
        if (current_ < 0) return;
        children_[static_cast<std::size_t>(current_)]->Next();
        FindSmallest();
    }

    bool Valid() const { return current_ >= 0; }

    Slice key() const {
        return children_[static_cast<std::size_t>(current_)]->key();
    }
    Slice value() const {
        return children_[static_cast<std::size_t>(current_)]->value();
    }

    Status status() const {
        for (const auto& c : children_) {
            Status s = c->status();
            if (!s.ok()) return s;
        }
        return STATUS(OK);
    }

private:
    void FindSmallest() {
        current_ = -1;
        for (std::size_t i = 0; i < children_.size(); ++i) {
            if (!children_[i]->Valid()) continue;
            if (current_ < 0 ||
                CompareInternalKey(children_[i]->key(),
                                   children_[static_cast<std::size_t>(current_)]->key()) < 0) {
                current_ = static_cast<int>(i);
            }
        }
    }

    std::vector<std::unique_ptr<InternalIter>> children_;
    int current_ = -1;
};

// User-key iterator: merge internal keys, apply snapshot, hide tombstones.
class DBIterator : public Iterator {
public:
    DBIterator(std::shared_ptr<MemTable> mem, std::shared_ptr<MemTable> imm, VersionPtr version,
               std::string dbname, Timestamp snapshot)
        : mem_(std::move(mem)),
          imm_(std::move(imm)),
          version_(std::move(version)),
          dbname_(std::move(dbname)),
          snapshot_(snapshot) {
        if (mem_) merge_.Add(std::make_unique<MemTableInternalIter>(mem_.get()));
        if (imm_) merge_.Add(std::make_unique<MemTableInternalIter>(imm_.get()));
        // L0 newest-first then L1: all tables participate in the merge; higher
        // sequence wins via internal-key order for the same user key.
        for (int level = 0; level < kNumLevels; ++level) {
            const auto& files = version_->files[level];
            if (level == 0) {
                for (auto it = files.rbegin(); it != files.rend(); ++it) {
                    AddSSTable(it->number);
                }
            } else {
                for (const auto& f : files) AddSSTable(f.number);
            }
        }
    }

    bool Valid() const override { return valid_; }

    void SeekToFirst() override {
        merge_.SeekToFirst();
        AdvanceToNextVisibleKey();
    }

    void Seek(const Slice& target) override {
        // Same lookup key shape as MemTable::Get / Version::Get.
        std::string ikey = MakeLookupKey(target, snapshot_);
        merge_.Seek(ikey);
        AdvanceToNextVisibleKey();
    }

    void Next() override {
        AdvanceToNextVisibleKey();
    }

    Slice key() const override { return Slice(user_key_); }
    Slice value() const override { return Slice(user_value_); }
    Status status() const override { return status_.ok() ? merge_.status() : status_; }

private:
    void AddSSTable(std::uint64_t number) {
        const std::string path = TableFileName(dbname_, number);
        std::unique_ptr<SSTable> table;
        Status s = SSTable::Open(path, &table);
        if (!s.ok()) {
            if (status_.ok()) status_ = s;
            return;
        }
        merge_.Add(std::make_unique<SSTableInternalIter>(std::move(table)));
    }

    // From the current merge position, skip internal keys that are invisible at
    // snapshot_ or are tombstones, and stop on the next live user-key/value pair.
    // Call after SeekToFirst/Seek (which place the merge cursor) or after the
    // previous entry has been consumed via Next.
    void AdvanceToNextVisibleKey() {
        valid_ = false;
        user_key_.clear();
        user_value_.clear();
        while (merge_.Valid()) {
            if (!status_.ok()) return;
            Status ms = merge_.status();
            if (!ms.ok()) {
                status_ = ms;
                return;
            }
            Slice ikey = merge_.key();
            if (ExtractSequence(ikey) > snapshot_) {
                merge_.Next();
                continue;
            }
            // First internal key for this user key at-or-before snapshot (highest seq).
            const Slice user_key_slice = ExtractUserKey(ikey);
            const ValueType type = ExtractValueType(ikey);
            const bool is_deletion = (type == kTypeDeletion);
            std::string value;
            if (!is_deletion) {
                Slice v = merge_.value();
                value.assign(v.data(), v.size());
            }
            const std::string this_user_key(user_key_slice.data(), user_key_slice.size());

            // Skip remaining versions of the same user key.
            merge_.Next();
            while (merge_.Valid()) {
                Slice next_ikey = merge_.key();
                if (ExtractUserKey(next_ikey).compare(Slice(this_user_key)) != 0) break;
                merge_.Next();
            }

            if (is_deletion) {
                continue;  // hide tombstone
            }
            user_key_ = this_user_key;
            user_value_ = std::move(value);
            valid_ = true;
            return;
        }
    }

    std::shared_ptr<MemTable> mem_;
    std::shared_ptr<MemTable> imm_;
    VersionPtr version_;
    std::string dbname_;
    Timestamp snapshot_;
    MergingInternalIter merge_;
    Status status_;
    bool valid_ = false;
    std::string user_key_;
    std::string user_value_;
};

}  // namespace

Iterator::~Iterator() = default;

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) {
    std::shared_ptr<MemTable> mem;
    std::shared_ptr<MemTable> imm;
    std::uint64_t snapshot;
    VersionPtr current;
    std::string dbname;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mem = mem_;
        imm = imm_;
        snapshot = options.snapshot != 0 ? options.snapshot : versions_->LastSequence();
        current = versions_->current();
        dbname = dbname_;
    }
    return std::unique_ptr<Iterator>(
        new DBIterator(std::move(mem), std::move(imm), std::move(current), std::move(dbname),
                       snapshot));
}

}  // namespace lsmkv
