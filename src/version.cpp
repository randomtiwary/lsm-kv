#include "lsmkv/version.h"

#include <algorithm>
#include <cstdlib>

#include "lsmkv/encoding.h"
#include "lsmkv/env.h"
#include "lsmkv/internal_key.h"

namespace lsmkv {
namespace {
// Tags for VersionEdit::Encode / DecodeFrom (see version.h for field layouts).
enum Tag : std::uint32_t {
    kNextFileNumber = 1,
    kLastSequence = 2,
    kLogNumber = 3,
    kAddFile = 4,
    kDeleteFile = 5
};

// MANIFEST record framing: fixed32(len) || encode(edit).
std::string FrameEdit(const std::string& rec) {
    std::string out;
    PutFixed32(&out, static_cast<std::uint32_t>(rec.size()));
    out.append(rec);
    return out;
}
}  // namespace

std::string VersionEdit::Encode() const {
    // Emit only fields that were explicitly set / populated.
    std::string out;
    if (has_next_file_number) {
        PutVarint32(&out, kNextFileNumber);
        PutVarint64(&out, next_file_number);
    }
    if (has_last_sequence) {
        PutVarint32(&out, kLastSequence);
        PutVarint64(&out, last_sequence);
    }
    if (has_log_number) {
        PutVarint32(&out, kLogNumber);
        PutVarint64(&out, log_number);
    }
    for (const auto& d : deleted_files) {
        PutVarint32(&out, kDeleteFile);
        PutVarint32(&out, static_cast<std::uint32_t>(d.first));
        PutVarint64(&out, d.second);
    }
    for (const auto& a : added_files) {
        PutVarint32(&out, kAddFile);
        PutVarint32(&out, static_cast<std::uint32_t>(a.first));
        PutVarint64(&out, a.second.number);
        PutVarint64(&out, a.second.file_size);
        PutLengthPrefixedSlice(&out, a.second.smallest);
        PutLengthPrefixedSlice(&out, a.second.largest);
    }
    return out;
}

Status VersionEdit::DecodeFrom(const Slice& src) {
    // Walk tag stream until input is consumed; unknown tags are corruption.
    Slice input = src;
    while (!input.empty()) {
        std::uint32_t tag = 0;
        if (!GetVarint32(&input, &tag)) return STATUS(Corruption, "edit bad tag");
        switch (tag) {
            case kNextFileNumber:
                if (!GetVarint64(&input, &next_file_number)) return STATUS(Corruption, "edit next file");
                has_next_file_number = true;
                break;
            case kLastSequence:
                if (!GetVarint64(&input, &last_sequence)) return STATUS(Corruption, "edit last seq");
                has_last_sequence = true;
                break;
            case kLogNumber:
                if (!GetVarint64(&input, &log_number)) return STATUS(Corruption, "edit log num");
                has_log_number = true;
                break;
            case kDeleteFile: {
                std::uint32_t level = 0;
                std::uint64_t number = 0;
                if (!GetVarint32(&input, &level) || !GetVarint64(&input, &number)) {
                    return STATUS(Corruption, "edit delete");
                }
                deleted_files.emplace_back(static_cast<int>(level), number);
                break;
            }
            case kAddFile: {
                std::uint32_t level = 0;
                FileMetaData f;
                Slice smallest, largest;
                if (!GetVarint32(&input, &level) || !GetVarint64(&input, &f.number) ||
                    !GetVarint64(&input, &f.file_size) || !GetLengthPrefixedSlice(&input, &smallest) ||
                    !GetLengthPrefixedSlice(&input, &largest)) {
                    return STATUS(Corruption, "edit add");
                }
                f.smallest = smallest.ToString();
                f.largest = largest.ToString();
                added_files.emplace_back(static_cast<int>(level), f);
                break;
            }
            default:
                return STATUS(Corruption, "unknown edit tag");
        }
    }
    return STATUS(OK);
}

bool FileOverlapsRange(const FileMetaData& f, const Slice& smallest, const Slice& largest) {
    if (ExtractUserKey(f.largest).compare(smallest) < 0) return false;
    if (ExtractUserKey(f.smallest).compare(largest) > 0) return false;
    return true;
}

Status Version::Get(const std::string& dbname, const Slice& user_key, std::uint64_t snapshot,
                    std::string* value) const {
    // Newest-first within each level so L0 flush order and overlapping files
    // resolve to the latest visible version at `snapshot`.
    std::string lkey = MakeLookupKey(user_key, snapshot);
    for (int level = 0; level < kNumLevels; ++level) {
        for (auto it = files[level].rbegin(); it != files[level].rend(); ++it) {
            const FileMetaData& f = *it;
            if (ExtractUserKey(f.smallest).compare(user_key) > 0) continue;
            if (ExtractUserKey(f.largest).compare(user_key) < 0) continue;
            std::unique_ptr<SSTable> table;
            Status s = SSTable::Open(TableFileName(dbname, f.number), &table);
            if (!s.ok()) return s;
            bool found = false;
            s = table->Get(lkey, value, &found);
            if (!s.ok() && !s.IsNotFound()) return s;
            if (found) return s;  // live value (OK) or tombstone (NotFound)
        }
    }
    return STATUS(NotFound);
}

VersionSet::VersionSet(std::string dbname)
    : dbname_(std::move(dbname)), current_(std::make_shared<Version>()) {}

Status VersionSet::WriteCurrentFile(const std::string& manifest_name) {
    // CURRENT holds only the manifest basename so recovery is relocate-friendly.
    return WriteStringToFileAtomic(Basename(manifest_name) + "\n", CurrentFileName());
}

Status VersionSet::WriteSnapshot(const std::string& manifest_path) {
    // One framed record describing the full live state (counters + every file).
    VersionEdit edit;
    edit.SetNextFile(next_file_number_);
    edit.SetLastSequence(last_sequence_);
    edit.SetLogNumber(log_number_);
    for (int level = 0; level < kNumLevels; ++level) {
        for (const auto& f : current_->files[level]) edit.AddFile(level, f);
    }
    return WriteStringToFile(FrameEdit(edit.Encode()), manifest_path);
}

Status VersionSet::NewDB() {
    Status s = CreateDir(dbname_);
    if (!s.ok()) return s;
    manifest_file_number_ = 1;
    next_file_number_ = 2;
    last_sequence_ = 0;
    log_number_ = 0;
    current_ = std::make_shared<Version>();
    std::string manifest = ManifestFileName(manifest_file_number_);
    s = WriteSnapshot(manifest);
    if (!s.ok()) return s;
    return WriteCurrentFile(manifest);
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
    std::lock_guard<std::mutex> lock(mu_);
    // 1) Merge scalar counters into VersionSet.
    if (edit->has_next_file_number) next_file_number_ = std::max(next_file_number_, edit->next_file_number);
    if (edit->has_last_sequence) last_sequence_ = std::max(last_sequence_, edit->last_sequence);
    if (edit->has_log_number) log_number_ = edit->log_number;

    // 2) Copy-on-write Version: apply deletes, then adds (sort L1 by user key).
    auto v = std::make_shared<Version>(*current_);
    for (const auto& d : edit->deleted_files) {
        auto& vec = v->files[d.first];
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [&](const FileMetaData& f) { return f.number == d.second; }),
                  vec.end());
    }
    for (const auto& a : edit->added_files) {
        v->files[a.first].push_back(a.second);
        if (a.first == 1) {
            std::sort(v->files[1].begin(), v->files[1].end(),
                      [](const FileMetaData& x, const FileMetaData& y) {
                          return ExtractUserKey(x.smallest).compare(ExtractUserKey(y.smallest)) < 0;
                      });
        }
    }
    // 3) Append framed edit to MANIFEST, then publish the new Version.
    Status s = AppendStringToFile(FrameEdit(edit->Encode()), ManifestFileName(manifest_file_number_));
    if (!s.ok()) return s;
    current_ = std::move(v);
    return STATUS(OK);
}

Status VersionSet::Recover() {
    // CURRENT -> MANIFEST path; replay every fixed32-framed VersionEdit in order.
    std::string current_data;
    Status s = ReadFileToString(CurrentFileName(), &current_data);
    if (!s.ok()) return STATUS(IOError, "missing CURRENT");
    std::string manifest_base = current_data;
    while (!manifest_base.empty() && (manifest_base.back() == '\n' || manifest_base.back() == '\r')) {
        manifest_base.pop_back();
    }
    std::string manifest = dbname_ + "/" + manifest_base;
    auto pos = manifest_base.find('-');
    if (pos != std::string::npos) {
        manifest_file_number_ = std::strtoull(manifest_base.c_str() + pos + 1, nullptr, 10);
    }
    std::string manifest_data;
    s = ReadFileToString(manifest, &manifest_data);
    if (!s.ok()) return STATUS(IOError, "missing manifest: " + manifest);
    current_ = std::make_shared<Version>();
    Slice input(manifest_data);
    while (!input.empty()) {
        if (input.size() < 4) return STATUS(Corruption, "truncated manifest header");
        std::uint32_t len = DecodeFixed32(input.data());
        input.remove_prefix(4);
        if (input.size() < len) return STATUS(Corruption, "truncated manifest rec");
        Slice rec(input.data(), len);
        input.remove_prefix(len);
        VersionEdit edit;
        s = edit.DecodeFrom(rec);
        if (!s.ok()) return s;
        if (edit.has_next_file_number) next_file_number_ = edit.next_file_number;
        if (edit.has_last_sequence) last_sequence_ = edit.last_sequence;
        if (edit.has_log_number) log_number_ = edit.log_number;
        for (const auto& d : edit.deleted_files) {
            auto& vec = current_->files[d.first];
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [&](const FileMetaData& f) { return f.number == d.second; }),
                      vec.end());
        }
        for (const auto& a : edit.added_files) current_->files[a.first].push_back(a.second);
    }
    // Level-1 files must be ordered by smallest user key for overlap checks.
    std::sort(current_->files[1].begin(), current_->files[1].end(),
              [](const FileMetaData& x, const FileMetaData& y) {
                  return ExtractUserKey(x.smallest).compare(ExtractUserKey(y.smallest)) < 0;
              });
    return STATUS(OK);
}

}  // namespace lsmkv
