#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lsmkv/env.h"
#include "lsmkv/sstable.h"
#include "lsmkv/status.h"

namespace lsmkv {

// LSM metadata: versions and the MANIFEST
// =======================================
//
// On-disk live file set is tracked with three cooperating types:
//
//   VersionEdit  — a delta ("add these SSTables, delete those, bump counters")
//   Version      — an immutable snapshot of which SSTables live at each level
//   VersionSet   — owns the current Version, assigns file numbers / sequences,
//                  and persists edits to MANIFEST / CURRENT
//
// Database directory layout used here:
//
//   dbname/
//     CURRENT              # one line: basename of the live manifest, e.g. MANIFEST-1
//     MANIFEST-<n>         # append-only log of encoded VersionEdit records
//     <number>.sst         # SSTable files referenced by Version::files
//     <number>.log         # WAL files (log_number_ points at the active one)
//     LOCK                 # optional lock file name helper
//
// Only two levels are implemented (kNumLevels = 2):
//   level 0 — flush outputs; files may overlap in key range; searched newest-first
//   level 1 — compaction outputs; non-overlapping, sorted by smallest user key

static const int kNumLevels = 2;

// VersionEdit
// -----------
// A single atomic change to version metadata. Built in memory by the DB layer
// (flush, compaction, recovery bookkeeping), then either:
//   - appended to MANIFEST via VersionSet::LogAndApply, or
//   - encoded as the initial snapshot record in VersionSet::NewDB / WriteSnapshot.
//
// Optional scalar fields use a has_* flag so encoding omits unset values and
// decoding leaves prior VersionSet state unchanged for missing tags:
//   next_file_number  — high-water mark for new .sst / .log numbers
//   last_sequence     — highest committed internal-key sequence
//   log_number        — WAL file number that is live after this edit
//
// File deltas:
//   added_files   : vector of (level, FileMetaData)
//                   FileMetaData = {number, file_size, smallest, largest}
//                   where smallest/largest are encoded internal keys.
//   deleted_files : vector of (level, file_number) removed from that level.
//
// On-disk record encoding (tag-prefixed, all integers little-endian varints
// except length-prefixed slices). Repeated tags are allowed (one per file):
//
//   tag 1 kNextFileNumber : varint64 next_file_number
//   tag 2 kLastSequence   : varint64 last_sequence
//   tag 3 kLogNumber      : varint64 log_number
//   tag 5 kDeleteFile     : varint32 level, varint64 file_number
//   tag 4 kAddFile        : varint32 level, varint64 number, varint64 file_size,
//                           lenpref(smallest), lenpref(largest)
//
// MANIFEST framing wraps each encoded edit as:
//   fixed32(payload_len) || payload bytes
// (no per-record CRC in this educational implementation).

struct VersionEdit {
    bool has_next_file_number = false;
    std::uint64_t next_file_number = 0;
    bool has_last_sequence = false;
    std::uint64_t last_sequence = 0;
    bool has_log_number = false;
    std::uint64_t log_number = 0;
    std::vector<std::pair<int, FileMetaData>> added_files;
    std::vector<std::pair<int, std::uint64_t>> deleted_files;  // level, number

    void SetNextFile(std::uint64_t n) {
        has_next_file_number = true;
        next_file_number = n;
    }
    void SetLastSequence(std::uint64_t s) {
        has_last_sequence = true;
        last_sequence = s;
    }
    void SetLogNumber(std::uint64_t n) {
        has_log_number = true;
        log_number = n;
    }
    void AddFile(int level, const FileMetaData& f) { added_files.emplace_back(level, f); }
    void DeleteFile(int level, std::uint64_t number) { deleted_files.emplace_back(level, number); }

    // Serialize / parse the tag stream described above (payload only).
    std::string Encode() const;
    Status DecodeFrom(const Slice& src);
};

// Version
// -------
// Immutable snapshot of the live SSTable set. VersionSet::current() returns a
// shared_ptr so readers can hold a stable view while a new Version is installed.
//
// files[level] lists FileMetaData for that level:
//   level 0: typically newest-last in vector order; Get() walks rbegin..rend
//            so more recently flushed tables are consulted first.
//   level 1: kept sorted by ExtractUserKey(smallest); ranges do not overlap.
//
// Get(dbname, user_key, snapshot, value) opens candidate SSTables whose
// [smallest, largest] user-key range covers user_key and probes each with a
// lookup key at `snapshot`. Stops at the first definitive hit (live value or
// tombstone). Returns NotFound if no table contains the key.

class Version {
public:
    std::vector<FileMetaData> files[kNumLevels];

    Status Get(const std::string& dbname, const Slice& user_key, std::uint64_t snapshot,
               std::string* value) const;
    int NumLevelFiles(int level) const {
        return static_cast<int>(files[level].size());
    }
};

using VersionPtr = std::shared_ptr<Version>;

// VersionSet
// ----------
// Process-wide owner of version state for one DB directory.
//
// Responsibilities:
//   - Allocate monotonically increasing file numbers (NextFileNumber).
//   - Track last_sequence_ and log_number_ for recovery / WAL selection.
//   - NewDB(): create directory, write an initial MANIFEST snapshot + CURRENT.
//   - LogAndApply(edit): under mu_, copy-on-write a new Version from current_,
//     apply deletes then adds (re-sort level 1), append the encoded edit to
//     MANIFEST, then publish current_.
//   - Recover(): read CURRENT -> open that MANIFEST, replay every framed edit
//     into an empty Version, restore counters, sort level 1.
//
// Threading: LogAndApply serializes writers with mu_. Readers use current()
// shared_ptr without holding mu_ (immutable Version contents).

class VersionSet {
public:
    explicit VersionSet(std::string dbname);

    VersionPtr current() const { return current_; }
    std::uint64_t NextFileNumber() { return next_file_number_++; }
    std::uint64_t LastSequence() const { return last_sequence_; }
    void SetLastSequence(std::uint64_t s) { last_sequence_ = s; }
    std::uint64_t LogNumber() const { return log_number_; }
    std::uint64_t ManifestFileNumber() const { return manifest_file_number_; }

    // Replay MANIFEST referenced by CURRENT into current_ and counters.
    Status Recover();
    // Persist *edit to MANIFEST and install the resulting Version as current_.
    Status LogAndApply(VersionEdit* edit);
    // Initialize a fresh DB directory with an empty snapshot manifest.
    Status NewDB();

    std::string TableFileName(std::uint64_t number) const {
        return lsmkv::TableFileName(dbname_, number);
    }
    std::string LogFileName(std::uint64_t number) const {
        return lsmkv::LogFileName(dbname_, number);
    }
    std::string ManifestFileName(std::uint64_t number) const {
        return lsmkv::ManifestFileName(dbname_, number);
    }
    std::string CurrentFileName() const { return lsmkv::CurrentFileName(dbname_); }
    std::string LockFileName() const { return lsmkv::LockFileName(dbname_); }

private:
    // Encode full current state as one framed VersionEdit record (used by NewDB).
    Status WriteSnapshot(const std::string& manifest_path);
    // Atomically replace CURRENT with a file containing manifest basename.
    Status WriteCurrentFile(const std::string& manifest_name);

    std::string dbname_;
    VersionPtr current_;
    std::uint64_t next_file_number_ = 2;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t log_number_ = 0;
    std::uint64_t manifest_file_number_ = 1;
    std::mutex mu_;
};

// True if f's user-key range [smallest, largest] intersects [smallest, largest]
// arguments (inclusive), using ExtractUserKey on the internal-key bounds.
bool FileOverlapsRange(const FileMetaData& f, const Slice& smallest, const Slice& largest);

}  // namespace lsmkv
