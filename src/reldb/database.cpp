#include "reldb/database.h"

#include "lsmkv/encoding.h"
#include "reldb/macros.h"
#include "reldb/txn.h"

namespace reldb {
namespace {

const char kNextTsKey[] = "m/next_ts";
const char kNextTxnKey[] = "m/next_txn";
const char kNextVidKey[] = "m/next_vid";

std::string EncodeTxnMeta(const TxnMeta& meta) {
    std::string out;
    out.push_back(static_cast<char>(meta.state));
    lsmkv::PutFixed64(&out, meta.commit_ts);
    return out;
}

lsmkv::Status DecodeTxnMeta(const std::string& bytes, TxnMeta* out) {
    if (bytes.size() != 1 + 8) {
        return STATUS(Corruption, "txn meta: bad length");
    }
    out->state = static_cast<TxnState>(static_cast<std::uint8_t>(bytes[0]));
    out->commit_ts = lsmkv::DecodeFixed64(bytes.data() + 1);
    return STATUS(OK);
}

}  // namespace

Database::Database(std::shared_ptr<lsmkv::DB> kv)
    : kv_(std::move(kv)),
      catalog_(std::make_unique<Catalog>(kv_)),
      store_(std::make_unique<MvccStore>(kv_)) {}

Database::~Database() = default;

lsmkv::Status Database::Open(const lsmkv::Options& options, const std::string& path,
                             Database** dbptr) {
    if (dbptr == nullptr) {
        return STATUS(InvalidArgument, "null dbptr");
    }
    lsmkv::DB* raw = nullptr;
    auto st = lsmkv::DB::Open(options, path, &raw);
    if (!st.ok()) return st;

    auto* db = new Database(std::shared_ptr<lsmkv::DB>(raw));
    st = db->InitOracles();
    if (!st.ok()) {
        delete db;
        return st;
    }
    *dbptr = db;
    return STATUS(OK);
}

lsmkv::Status Database::InitOracles() {
    auto load = [&](const char* key, Timestamp* dst, Timestamp default_v) -> lsmkv::Status {
        std::string bytes;
        auto st = kv_->Get(lsmkv::ReadOptions(), key, &bytes);
        if (st.IsNotFound()) {
            *dst = default_v;
            return STATUS(OK);
        }
        RELDB_RETURN_NOT_OK(st);
        if (bytes.size() != 8) {
            return STATUS(Corruption, std::string(key) + ": bad length");
        }
        *dst = lsmkv::DecodeFixed64(bytes.data());
        if (*dst == 0) *dst = default_v;
        return STATUS(OK);
    };
    RELDB_RETURN_NOT_OK(load(kNextTsKey, &next_ts_, 1));
    RELDB_RETURN_NOT_OK(load(kNextTxnKey, &next_txn_id_, 1));
    RELDB_RETURN_NOT_OK(load(kNextVidKey, &next_version_id_, 1));
    return PersistOracles();
}

lsmkv::Status Database::PersistOracles() {
    auto put = [&](const char* key, Timestamp v) {
        std::string bytes;
        lsmkv::PutFixed64(&bytes, v);
        return kv_->Put(lsmkv::WriteOptions(), key, bytes);
    };
    RELDB_RETURN_NOT_OK(put(kNextTsKey, next_ts_));
    RELDB_RETURN_NOT_OK(put(kNextTxnKey, next_txn_id_));
    RELDB_RETURN_NOT_OK(put(kNextVidKey, next_version_id_));
    return STATUS(OK);
}

lsmkv::Status Database::CreateTable(const TableSchema& schema) {
    return catalog_->CreateTable(schema);
}

lsmkv::Status Database::GetTxnMeta(TxnId id, TxnMeta* out) const {
    if (out == nullptr) {
        return STATUS(InvalidArgument, "null out");
    }
    std::string bytes;
    auto st = kv_->Get(lsmkv::ReadOptions(), TxnKey(id), &bytes);
    if (st.IsNotFound()) {
        return STATUS(NotFound, "txn not found");
    }
    RELDB_RETURN_NOT_OK(st);
    return DecodeTxnMeta(bytes, out);
}

lsmkv::Status Database::PutTxnMeta(TxnId id, const TxnMeta& meta) {
    return kv_->Put(lsmkv::WriteOptions(), TxnKey(id), EncodeTxnMeta(meta));
}

lsmkv::Status Database::Begin(Transaction** txn) {
    if (txn == nullptr) {
        return STATUS(InvalidArgument, "null txn");
    }
    std::lock_guard<std::mutex> lock(mu_);
    const TxnId id = next_txn_id_++;
    const Timestamp start_ts = next_ts_ - 1;
    RELDB_RETURN_NOT_OK(PersistOracles());

    TxnMeta meta;
    meta.state = TxnState::kOpen;
    meta.commit_ts = 0;
    RELDB_RETURN_NOT_OK(PutTxnMeta(id, meta));

    *txn = new Transaction(this, id, start_ts);
    return STATUS(OK);
}

lsmkv::Status Database::RestoreWrittenHeads(Transaction* txn) {
    for (auto it = txn->written_.rbegin(); it != txn->written_.rend(); ++it) {
        Timestamp head = 0;
        auto st = store_->GetLatestVersionId(it->table, it->pk, &head);
        if (st.IsNotFound()) continue;
        RELDB_RETURN_NOT_OK(st);
        if (head != it->version_id) continue;

        VersionRecord rec;
        RELDB_RETURN_NOT_OK(store_->GetVersion(it->table, it->pk, it->version_id, &rec));
        if (rec.prev_id == 0) {
            RELDB_RETURN_NOT_OK(store_->ClearHead(it->table, it->pk));
        } else {
            RELDB_RETURN_NOT_OK(store_->SetHead(it->table, it->pk, rec.prev_id));
        }
    }
    return STATUS(OK);
}

lsmkv::Status Database::CommitTransaction(Transaction* txn) {
    std::lock_guard<std::mutex> lock(mu_);
    if (txn->finished_) {
        return STATUS(InvalidArgument, "transaction already finished");
    }

    // First-committer-wins re-check before allocating commit_ts.
    for (const auto& w : txn->written_) {
        Timestamp head = 0;
        RELDB_RETURN_NOT_OK(store_->GetLatestVersionId(w.table, w.pk, &head));
        if (head != w.version_id) {
            RELDB_RETURN_NOT_OK(RestoreWrittenHeads(txn));
            TxnMeta aborted;
            aborted.state = TxnState::kAborted;
            RELDB_RETURN_NOT_OK(PutTxnMeta(txn->txn_id_, aborted));
            txn->finished_ = true;
            return STATUS(Conflict, "write-write conflict: head moved");
        }

        VersionRecord rec;
        RELDB_RETURN_NOT_OK(store_->GetVersion(w.table, w.pk, w.version_id, &rec));
        if (rec.prev_id != 0) {
            VersionRecord prev;
            auto st = store_->GetVersion(w.table, w.pk, rec.prev_id, &prev);
            if (st.ok() && !prev.is_provisional() && prev.start_ts > txn->start_ts_) {
                RELDB_RETURN_NOT_OK(RestoreWrittenHeads(txn));
                TxnMeta aborted;
                aborted.state = TxnState::kAborted;
                RELDB_RETURN_NOT_OK(PutTxnMeta(txn->txn_id_, aborted));
                txn->finished_ = true;
                return STATUS(Conflict, "write-write conflict: key committed after snapshot");
            }
            if (!st.ok() && !st.IsNotFound()) return st;
        }
    }

    const Timestamp commit_ts = next_ts_++;

    for (const auto& w : txn->written_) {
        VersionRecord rec;
        RELDB_RETURN_NOT_OK(store_->GetVersion(w.table, w.pk, w.version_id, &rec));
        if (rec.created_by != txn->txn_id_ || !rec.is_provisional()) {
            return STATUS(Corruption, "commit: unexpected version state");
        }

        if (rec.prev_id != 0) {
            VersionRecord prev;
            auto st = store_->GetVersion(w.table, w.pk, rec.prev_id, &prev);
            if (st.ok() && !prev.is_provisional() && prev.end_ts == 0) {
                prev.end_ts = commit_ts;
                RELDB_RETURN_NOT_OK(store_->PutVersionValue(w.table, w.pk, prev));
            } else if (!st.ok() && !st.IsNotFound()) {
                return st;
            }
        }

        rec.start_ts = commit_ts;
        RELDB_RETURN_NOT_OK(store_->PutVersionValue(w.table, w.pk, rec));
    }

    TxnMeta meta;
    meta.state = TxnState::kCommitted;
    meta.commit_ts = commit_ts;
    RELDB_RETURN_NOT_OK(PutTxnMeta(txn->txn_id_, meta));
    RELDB_RETURN_NOT_OK(PersistOracles());

    txn->finished_ = true;
    return STATUS(OK);
}

lsmkv::Status Database::AbortTransaction(Transaction* txn) {
    std::lock_guard<std::mutex> lock(mu_);
    if (txn->finished_) {
        return STATUS(OK);
    }

    RELDB_RETURN_NOT_OK(RestoreWrittenHeads(txn));

    TxnMeta meta;
    meta.state = TxnState::kAborted;
    meta.commit_ts = 0;
    RELDB_RETURN_NOT_OK(PutTxnMeta(txn->txn_id_, meta));

    txn->finished_ = true;
    return STATUS(OK);
}

}  // namespace reldb
