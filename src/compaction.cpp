#include "lsmkv/compaction.h"

#include <map>

#include "lsmkv/internal_key.h"
#include "lsmkv/sstable.h"

namespace lsmkv {

Status CompactLevel0(const Options& options, const std::string& dbname, Version* current,
                     std::uint64_t output_number, CompactionResult* result) {
    result->inputs.clear();
    std::vector<std::unique_ptr<SSTable>> tables;
    std::vector<std::unique_ptr<SSTable::Iterator>> iters;

    Slice smallest_user, largest_user;
    bool first = true;
    for (const auto& f : current->files[0]) {
        result->inputs.emplace_back(0, f.number);
        std::unique_ptr<SSTable> t;
        Status s = SSTable::Open(dbname + "/" + std::to_string(f.number) + ".sst", &t);
        if (!s.ok()) return s;
        auto it = t->NewIterator();
        it->SeekToFirst();
        iters.push_back(std::move(it));
        tables.push_back(std::move(t));
        Slice suser = ExtractUserKey(f.smallest);
        Slice luser = ExtractUserKey(f.largest);
        if (first || suser.compare(smallest_user) < 0) smallest_user = suser;
        if (first || luser.compare(largest_user) > 0) largest_user = luser;
        first = false;
    }
    // Keep string storage alive
    std::string smallest_storage = smallest_user.ToString();
    std::string largest_storage = largest_user.ToString();
    smallest_user = smallest_storage;
    largest_user = largest_storage;

    for (const auto& f : current->files[1]) {
        if (!FileOverlapsRange(f, smallest_user, largest_user)) continue;
        result->inputs.emplace_back(1, f.number);
        std::unique_ptr<SSTable> t;
        Status s = SSTable::Open(dbname + "/" + std::to_string(f.number) + ".sst", &t);
        if (!s.ok()) return s;
        auto it = t->NewIterator();
        it->SeekToFirst();
        iters.push_back(std::move(it));
        tables.push_back(std::move(t));
    }

    // Merge by internal key order; keep newest per user key.
    std::string out_path = dbname + "/" + std::to_string(output_number) + ".sst";
    SSTableBuilder builder(options, out_path);
    Status s = builder.Open();
    if (!s.ok()) return s;

    std::string last_user;
    bool has_last = false;
    while (true) {
        int best = -1;
        for (int i = 0; i < static_cast<int>(iters.size()); ++i) {
            if (!iters[i] || !iters[i]->Valid()) continue;
            if (best < 0 || CompareInternalKey(iters[i]->key(), iters[best]->key()) < 0) best = i;
        }
        if (best < 0) break;
        Slice ikey = iters[best]->key();
        Slice user = ExtractUserKey(ikey);
        if (!has_last || user.compare(Slice(last_user)) != 0) {
            // Emit only the newest version per user key (seq descends within a user key).
            // Keep tombstones so readers still observe deletes if an older file is missed.
            builder.Add(ikey, iters[best]->value());
            last_user = user.ToString();
            has_last = true;
        }
        iters[best]->Next();
    }
    s = builder.Finish(&result->output);
    if (!s.ok()) return s;
    result->output.number = output_number;
    return STATUS(OK);
}

}  // namespace lsmkv
