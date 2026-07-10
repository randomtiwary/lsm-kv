#include "reldb/database.h"

#include "lsmkv/encoding.h"
#include "reldb/txn.h"

namespace reldb {
namespace {

const char kNextTsKey[] = "m/next_ts";

}  // namespace

Database::Database(std::shared_ptr<lsmkv::DB> kv)
    : kv_(std::move(kv)),
      catalog_(std::make_unique<Catalog>(kv_)),
      store_(std::make_unique<MvccStore>(kv_)) {}

Database::~Database() = default;

lsmkv::Status Database::Open(const lsmkv::Options& options, const std::string& path,
                             Database** dbptr) {
    if (dbptr == nullptr) {
        return lsmkv::Status::InvalidArgument("null dbptr");
    }
    lsmkv::DB* raw = nullptr;
    auto st = lsmkv::DB::Open(options, path, &raw);
    if (!st.ok()) return st;

    auto* db = new Database(std::shared_ptr<lsmkv::DB>(raw));
    st = db->InitOracle();
    if (!st.ok()) {
        delete db;
        return st;
    }
    *dbptr = db;
    return lsmkv::Status::OK();
}

lsmkv::Status Database::InitOracle() {
    std::string bytes;
    auto st = kv_->Get(lsmkv::ReadOptions(), kNextTsKey, &bytes);
    if (st.IsNotFound()) {
        next_ts_ = 1;
        return PersistOracle();
    }
    if (!st.ok()) return st;
    if (bytes.size() != 8) {
        return lsmkv::Status::Corruption("next_ts: bad length");
    }
    next_ts_ = lsmkv::DecodeFixed64(bytes.data());
    if (next_ts_ == 0) {
        next_ts_ = 1;
    }
    return lsmkv::Status::OK();
}

lsmkv::Status Database::PersistOracle() {
    std::string bytes;
    lsmkv::PutFixed64(&bytes, next_ts_);
    return kv_->Put(lsmkv::WriteOptions(), kNextTsKey, bytes);
}

lsmkv::Status Database::CreateTable(const TableSchema& schema) {
    return catalog_->CreateTable(schema);
}

lsmkv::Status Database::Begin(Transaction** txn) {
    if (txn == nullptr) {
        return lsmkv::Status::InvalidArgument("null txn");
    }
    Timestamp start_ts = 0;
    {
        std::lock_guard<std::mutex> lock(commit_mu_);
        start_ts = next_ts_ - 1;
    }
    *txn = new Transaction(this, start_ts);
    return lsmkv::Status::OK();
}

lsmkv::Status Database::CommitTransaction(Transaction* txn) {
    std::lock_guard<std::mutex> lock(commit_mu_);
    if (txn->finished_) {
        return lsmkv::Status::InvalidArgument("transaction already finished");
    }
    if (txn->write_set_.empty()) {
        txn->finished_ = true;
        return lsmkv::Status::OK();
    }

    const Timestamp commit_ts = next_ts_;

    // Phase 1: conflict checks and semantic checks (no writes yet).
    for (const auto& kv : txn->write_set_) {
        const auto& e = kv.second;
        Timestamp latest = 0;
        auto st = store_->GetLatestStartTs(e.table, e.pk, &latest);
        if (!st.ok() && !st.IsNotFound()) return st;

        if (st.ok() && latest > txn->start_ts_) {
            txn->finished_ = true;
            return lsmkv::Status::Conflict("write-write conflict on " + e.table);
        }

        if (e.op == Transaction::WriteOp::kInsert) {
            if (st.ok()) {
                VersionRecord cur;
                auto gst = store_->GetVersion(e.table, e.pk, latest, &cur);
                if (!gst.ok()) return gst;
                // Live row exists if the latest version is still open and not a tombstone.
                if (!cur.is_tombstone && cur.end_ts == 0) {
                    txn->finished_ = true;
                    return lsmkv::Status::InvalidArgument("duplicate primary key");
                }
            }
        } else {
            // Update / Delete require a currently live row.
            if (st.IsNotFound()) {
                txn->finished_ = true;
                return lsmkv::Status::NotFound("row not found for update/delete");
            }
            VersionRecord cur;
            auto gst = store_->GetVersion(e.table, e.pk, latest, &cur);
            if (!gst.ok()) return gst;
            if (cur.is_tombstone || cur.end_ts != 0) {
                txn->finished_ = true;
                return lsmkv::Status::NotFound("row not found for update/delete");
            }
        }
    }

    // Phase 2: apply versions at commit_ts.
    for (const auto& kv : txn->write_set_) {
        const auto& e = kv.second;
        Timestamp prev_ts = 0;
        Timestamp latest = 0;
        auto st = store_->GetLatestStartTs(e.table, e.pk, &latest);
        if (st.ok()) {
            prev_ts = latest;
            // Supersede the current live version.
            st = store_->CloseVersion(e.table, e.pk, latest, commit_ts);
            if (!st.ok()) return st;
        } else if (!st.IsNotFound()) {
            return st;
        }

        VersionRecord rec;
        rec.start_ts = commit_ts;
        rec.end_ts = 0;
        rec.prev_ts = prev_ts;
        if (e.op == Transaction::WriteOp::kDelete) {
            rec.is_tombstone = true;
        } else {
            rec.is_tombstone = false;
            rec.payload = e.row.Encode();
        }
        st = store_->PutVersion(e.table, e.pk, rec);
        if (!st.ok()) return st;
    }

    next_ts_ = commit_ts + 1;
    auto st = PersistOracle();
    if (!st.ok()) return st;

    txn->finished_ = true;
    return lsmkv::Status::OK();
}

void Database::FinishTransaction(Transaction* txn) {
    std::lock_guard<std::mutex> lock(commit_mu_);
    txn->finished_ = true;
}

}  // namespace reldb
