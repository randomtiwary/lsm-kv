#include "db_impl.h"

#include <fstream>
#include <algorithm>
#include <optional>
#include <cstdio>
#include "lsmkv/compaction.h"
#include "lsmkv/env.h"
#include "lsmkv/internal_key.h"

namespace lsmkv {

DB::~DB() = default;

DBImpl::DBImpl(const Options& options, std::string dbname)
    : options_(options), dbname_(std::move(dbname)), versions_(new VersionSet(dbname_)) {}

DBImpl::~DBImpl() {
    shutting_down_.store(true);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        bg_cv_.notify_all();
        bg_cv_.wait(lock, [&] { return !bg_working_; });
        // Synchronously finish any pending flush so data is durable on close.
        while (imm_ != nullptr) {
            lock.unlock();
            Status s = FlushMemTable();
            lock.lock();
            if (!s.ok()) bg_error_ = s;
        }
    }
    if (bg_thread_.joinable()) bg_thread_.join();
    if (wal_) (void)wal_->Close();
}

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
    *dbptr = nullptr;
    auto* impl = new DBImpl(options, name);
    Status s = impl->Open();
    if (!s.ok()) {
        delete impl;
        return s;
    }
    *dbptr = impl;
    return STATUS(OK);
}

Status DBImpl::Open() {
    const bool exists = PathExists(dbname_) && PathExists(versions_->CurrentFileName());
    if (exists && options_.error_if_exists) {
        return STATUS(InvalidArgument, "database already exists");
    }
    if (!exists) {
        if (!options_.create_if_missing) return STATUS(InvalidArgument, "database does not exist");
        Status s = versions_->NewDB();
        if (!s.ok()) return s;
    } else {
        Status s = versions_->Recover();
        if (!s.ok()) return s;
    }

    mem_.reset(new MemTable());
    logfile_number_ = versions_->NextFileNumber();
    wal_.reset(new WalWriter(versions_->LogFileName(logfile_number_)));
    Status s = wal_->Open();
    if (!s.ok()) return s;

    if (exists && versions_->LogNumber() > 0) {
        s = RecoverLogFile(versions_->LogNumber());
        if (!s.ok()) return s;
    }
    // Also recover current log if different.
    if (logfile_number_ != versions_->LogNumber()) {
        // fresh log already open; prior log recovered above
    }

    VersionEdit edit;
    edit.SetLogNumber(logfile_number_);
    edit.SetNextFile(versions_->NextFileNumber());
    s = versions_->LogAndApply(&edit);
    if (!s.ok()) return s;

    bg_thread_ = std::thread(&DBImpl::BackgroundThreadMain, this);
    return STATUS(OK);
}

Status DBImpl::RecoverLogFile(std::uint64_t log_number) {
    WalReader reader(versions_->LogFileName(log_number));
    Status s = reader.Open();
    if (!s.ok()) {
        if (s.IsIOError()) return STATUS(OK);  // missing log is fine for empty DB
        return s;
    }
    std::uint64_t max_seq = versions_->LastSequence();
    while (true) {
        std::string record;
        s = reader.ReadRecord(&record);
        if (s.IsNotFound()) break;
        if (!s.ok()) return s;
        std::uint64_t seq = 0;
        std::vector<BatchEntry> entries;
        s = DecodeWriteBatch(record, &seq, &entries);
        if (!s.ok()) return s;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            mem_->Add(seq + i, e.first, e.second.first, e.second.second);
            max_seq = std::max(max_seq, seq + i);
        }
    }
    reader.Close();
    versions_->SetLastSequence(max_seq);
    return STATUS(OK);
}

Status DBImpl::Write(const WriteOptions& options, ValueType type, const Slice& key,
                     const Slice& value) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!bg_error_.ok()) return bg_error_;
    Status s = MakeRoomForWrite(lock);
    if (!s.ok()) return s;
    std::uint64_t seq = versions_->LastSequence() + 1;
    versions_->SetLastSequence(seq);
    BatchEntry entry{type, {key.ToString(), value.ToString()}};
    std::string record = EncodeWriteBatch(seq, {entry});
    s = wal_->AddRecord(record);
    if (!s.ok()) return s;
    if (options.sync) {
        s = wal_->Sync();
        if (!s.ok()) return s;
    }
    mem_->Add(seq, type, key, value);
    if (mem_->ApproximateMemoryUsage() >= options_.write_buffer_size) {
        flush_scheduled_ = true;
        MaybeScheduleWork();
    }
    return STATUS(OK);
}

Status DBImpl::Put(const WriteOptions& options, const Slice& key, const Slice& value) {
    return Write(options, kTypeValue, key, value);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
    return Write(options, kTypeDeletion, key, Slice());
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
    std::shared_ptr<MemTable> mem;
    std::shared_ptr<MemTable> imm;
    std::uint64_t snapshot;
    VersionPtr current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mem = mem_;
        imm = imm_;
        snapshot = options.snapshot != 0 ? options.snapshot : versions_->LastSequence();
        current = versions_->current();
    }
    std::optional<std::string> opt_value;
    bool found = false;
    Status s = mem->Get(key, snapshot, &opt_value, &found);
    if (!s.ok()) return s;
    if (found) {
        if (!opt_value.has_value()) return STATUS(NotFound);
        *value = std::move(*opt_value);
        return STATUS(OK);
    }
    if (imm) {
        s = imm->Get(key, snapshot, &opt_value, &found);
        if (!s.ok()) return s;
        if (found) {
            if (!opt_value.has_value()) return STATUS(NotFound);
            *value = std::move(*opt_value);
            return STATUS(OK);
        }
    }
    return current->Get(dbname_, key, snapshot, value);
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock) {
    while (true) {
        if (!bg_error_.ok()) return bg_error_;
        if (mem_->ApproximateMemoryUsage() < options_.write_buffer_size) return STATUS(OK);
        if (imm_ != nullptr) {
            bg_cv_.wait(lock);
            continue;
        }
        imm_ = mem_;
        mem_.reset(new MemTable());
        logfile_number_ = versions_->NextFileNumber();
        wal_.reset(new WalWriter(versions_->LogFileName(logfile_number_)));
        Status s = wal_->Open();
        if (!s.ok()) return s;
        VersionEdit edit;
        edit.SetLogNumber(logfile_number_);
        s = versions_->LogAndApply(&edit);
        if (!s.ok()) return s;
        flush_scheduled_ = true;
        MaybeScheduleWork();
        return STATUS(OK);
    }
}

void DBImpl::MaybeScheduleWork() {
    bg_cv_.notify_all();
}

void DBImpl::BackgroundThreadMain() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!shutting_down_.load()) {
        if (!flush_scheduled_ && !compaction_scheduled_) {
            bg_cv_.wait(lock);
            continue;
        }
        bg_working_ = true;
        if (flush_scheduled_ && imm_ != nullptr) {
            flush_scheduled_ = false;
            lock.unlock();
            Status s = FlushMemTable();
            lock.lock();
            if (!s.ok()) bg_error_ = s;
            compaction_scheduled_ = true;
        } else if (compaction_scheduled_) {
            compaction_scheduled_ = false;
            lock.unlock();
            Status s = CompactIfNeeded();
            lock.lock();
            if (!s.ok()) bg_error_ = s;
        } else {
            flush_scheduled_ = false;
            compaction_scheduled_ = false;
        }
        bg_working_ = false;
        bg_cv_.notify_all();
    }
}

Status DBImpl::FlushMemTable() {
    std::shared_ptr<MemTable> imm;
    std::uint64_t file_number;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!imm_) return STATUS(OK);
        imm = imm_;
        file_number = versions_->NextFileNumber();
    }
    FileMetaData meta;
    std::string path = versions_->TableFileName(file_number);
    Status s = BuildTableFromMemTable(options_, path, *imm, file_number, &meta);
    if (!s.ok()) return s;
    if (meta.file_size > 0 && !meta.smallest.empty()) {
        VersionEdit edit;
        edit.AddFile(0, meta);
        edit.SetLastSequence(versions_->LastSequence());
        std::lock_guard<std::mutex> lock(mutex_);
        s = versions_->LogAndApply(&edit);
        if (!s.ok()) return s;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        imm_.reset();
    }
    return STATUS(OK);
}

Status DBImpl::CompactIfNeeded() {
    VersionPtr current;
    int l0_files;
    std::uint64_t out_number;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = versions_->current();
        l0_files = current->NumLevelFiles(0);
        if (l0_files < options_.level0_compaction_trigger) return STATUS(OK);
        out_number = versions_->NextFileNumber();
    }
    CompactionResult result;
    Status s = CompactLevel0(options_, dbname_, current.get(), out_number, &result);
    if (!s.ok()) return s;
    VersionEdit edit;
    for (const auto& in : result.inputs) edit.DeleteFile(in.first, in.second);
    if (result.output.file_size > 0 && !result.output.smallest.empty()) {
        edit.AddFile(1, result.output);
    }
    edit.SetLastSequence(versions_->LastSequence());
    std::lock_guard<std::mutex> lock(mutex_);
    s = versions_->LogAndApply(&edit);
    if (!s.ok()) return s;
    for (const auto& in : result.inputs) {
        std::string path = dbname_ + "/" + std::to_string(in.second) + ".sst";
        std::remove(path.c_str());
    }
    return STATUS(OK);
}

}  // namespace lsmkv
