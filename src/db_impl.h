#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "lsmkv/db.h"
#include "lsmkv/memtable.h"
#include "lsmkv/version.h"
#include "lsmkv/wal.h"

namespace lsmkv {

// Private implementation behind the public DB handle.
//
// Write path (always under mutex_):
//   MakeRoomForWrite -> append WAL record -> mem_->Add
// When mem_ fills, it is swapped to imm_ and a background flush builds an L0
// SSTable from that immutable table while writers continue on a fresh mem_.
//
// Read path snapshots mem_, imm_, and VersionSet::current() under mutex_, then
// searches mem -> imm -> SSTables without holding the lock.

class DBImpl : public DB {
public:
    DBImpl(const Options& options, std::string dbname);
    ~DBImpl() override;

    Status Open();
    Status Put(const WriteOptions& options, const Slice& key, const Slice& value) override;
    Status Delete(const WriteOptions& options, const Slice& key) override;
    Status Get(const ReadOptions& options, const Slice& key, std::string* value) override;

private:
    Status Write(const WriteOptions& options, ValueType type, const Slice& key, const Slice& value);
    // Ensure mem_ has capacity; may install imm_ and schedule a flush.
    Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock);
    // Build an L0 table from imm_ (called without holding mutex_ for the heavy work).
    Status FlushMemTable();
    Status CompactIfNeeded();
    Status RecoverLogFile(std::uint64_t log_number);
    void BackgroundThreadMain();
    void MaybeScheduleWork();

    Options options_;
    std::string dbname_;

    // Owns MANIFEST/CURRENT, live SSTable set, file numbers, and last sequence.
    std::unique_ptr<VersionSet> versions_;

    // Active memtable: the only table that accepts Put/Delete via mem_->Add.
    std::shared_ptr<MemTable> mem_;
    // Immutable memtable awaiting flush. Set by swapping the old mem_ under
    // mutex_; never receives Add afterward. nullptr when no flush is pending.
    // shared_ptr so FlushMemTable / Get can hold a stable reference after
    // releasing mutex_.
    std::shared_ptr<MemTable> imm_;

    // WAL for the current mem_. Rotated when mem_ is swapped to imm_ so recovery
    // can replay only logs that are not yet reflected in SSTables.
    std::unique_ptr<WalWriter> wal_;
    // File number of wal_ / the active log (also stored in VersionSet::log_number_).
    std::uint64_t logfile_number_ = 0;

    // Serializes writers and all transitions of mem_/imm_/wal_/flags. Readers
    // take it only briefly to copy shared_ptr snapshots.
    std::mutex mutex_;
    // Signaled when a flush/compaction finishes or work is scheduled; writers
    // wait here if mem_ is full while imm_ is still flushing.
    std::condition_variable bg_cv_;
    std::thread bg_thread_;
    std::atomic<bool> shutting_down_{false};
    // True while the background thread is inside FlushMemTable or CompactIfNeeded.
    bool bg_working_ = false;
    // Latch set when imm_ is installed (or mem_ hits the write buffer limit);
    // cleared when the bg thread picks up the flush.
    bool flush_scheduled_ = false;
    // Set after a successful flush so the bg thread considers L0->L1 compaction.
    bool compaction_scheduled_ = false;
    // Sticky error from background work; subsequent writes fail fast with it.
    Status bg_error_;
};

}  // namespace lsmkv
