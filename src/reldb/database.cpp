#include "reldb/database.h"

#include "lsmkv/encoding.h"
#include "lsmkv/slice.h"
#include "reldb/macros.h"
#include "reldb/txn.h"

namespace reldb {
namespace {

const char kNextTsKey[] = "m/next_ts";
const char kNextTxnKey[] = "m/next_txn";
const char kNextVidKey[] = "m/next_vid";

// TxnMeta wire format:
//   state(1) + commit_ts(8)  [legacy / no write list]
//   state(1) + commit_ts(8) + n(4) + n * (len-prefixed table, len-prefixed pk_hex, version_id(8))
std::string EncodeTxnMeta(const TxnMeta& meta) {
    std::string out;
    out.push_back(static_cast<char>(meta.state));
    lsmkv::PutFixed64(&out, meta.commit_ts);
    if (!meta.writes.empty()) {
        lsmkv::PutFixed32(&out, static_cast<std::uint32_t>(meta.writes.size()));
        for (const auto& w : meta.writes) {
            lsmkv::PutLengthPrefixedSlice(&out, lsmkv::Slice(w.table));
            const std::string pk_hex = EncodePkForKey(w.pk);
            lsmkv::PutLengthPrefixedSlice(&out, lsmkv::Slice(pk_hex));
            lsmkv::PutFixed64(&out, w.version_id);
        }
    }
    return out;
}

lsmkv::Status DecodeTxnMeta(const std::string& bytes, TxnMeta* out) {
    if (bytes.size() < 1 + 8) {
        return STATUS(Corruption, "txn meta: bad length");
    }
    out->state = static_cast<TxnState>(static_cast<std::uint8_t>(bytes[0]));
    out->commit_ts = lsmkv::DecodeFixed64(bytes.data() + 1);
    out->writes.clear();
    if (bytes.size() == 1 + 8) {
        return STATUS(OK);
    }
    lsmkv::Slice input(bytes.data() + 9, bytes.size() - 9);
    std::uint32_t n = 0;
    if (!lsmkv::GetFixed32(&input, &n)) {
        return STATUS(Corruption, "txn meta: bad write count");
    }
    out->writes.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        lsmkv::Slice table_sl;
        lsmkv::Slice pk_hex_sl;
        std::uint64_t vid = 0;
        if (!lsmkv::GetLengthPrefixedSlice(&input, &table_sl) ||
            !lsmkv::GetLengthPrefixedSlice(&input, &pk_hex_sl) ||
            !lsmkv::GetFixed64(&input, &vid)) {
            return STATUS(Corruption, "txn meta: bad write entry");
        }
        TxnWrite w;
        w.table.assign(table_sl.data(), table_sl.size());
        RELDB_RETURN_NOT_OK(DecodePkFromKey(std::string(pk_hex_sl.data(), pk_hex_sl.size()), &w.pk));
        w.version_id = vid;
        out->writes.push_back(std::move(w));
    }
    if (!input.empty()) {
        return STATUS(Corruption, "txn meta: trailing bytes");
    }
    return STATUS(OK);
}

}  // namespace

Database::Database(std::shared_ptr<lsmkv::DB> kv)
    : kv_(std::move(kv)),
      catalog_(std::make_shared<Catalog>(kv_)),
      store_(std::make_shared<MvccStore>(kv_)) {}

Database::~Database() = default;

lsmkv::Status Database::Open(const lsmkv::Options& options, const std::string& path,
                             std::unique_ptr<Database>* dbptr) {
    if (dbptr == nullptr) {
        return STATUS(InvalidArgument, "null dbptr");
    }
    lsmkv::DB* raw = nullptr;
    RELDB_RETURN_NOT_OK(lsmkv::DB::Open(options, path, &raw));
    auto db = std::unique_ptr<Database>(new Database(std::shared_ptr<lsmkv::DB>(raw)));
    auto st = db->InitOracles();
    if (!st.ok()) {
        return st;
    }
    st = db->RecoverTxns();
    if (!st.ok()) {
        return st;
    }
    *dbptr = std::move(db);
    return STATUS(OK);
}

lsmkv::Status Database::InitOracles() {
    auto load = [&](const char* key, Timestamp* dst, Timestamp def) -> lsmkv::Status {
        std::string bytes;
        auto st = kv_->Get(lsmkv::ReadOptions(), key, &bytes);
        if (st.IsNotFound()) {
            *dst = def;
            return STATUS(OK);
        }
        RELDB_RETURN_NOT_OK(st);
        if (bytes.size() != 8) {
            return STATUS(Corruption, std::string(key) + ": bad length");
        }
        *dst = lsmkv::DecodeFixed64(bytes.data());
        if (*dst == 0) *dst = def;
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
    if (out == nullptr) return STATUS(InvalidArgument, "null out");
    std::string bytes;
    auto st = kv_->Get(lsmkv::ReadOptions(), TxnKey(id), &bytes);
    if (st.IsNotFound()) return STATUS(NotFound, "txn not found");
    RELDB_RETURN_NOT_OK(st);
    return DecodeTxnMeta(bytes, out);
}

lsmkv::Status Database::PutTxnMeta(TxnId id, const TxnMeta& meta) {
    return kv_->Put(lsmkv::WriteOptions(), TxnKey(id), EncodeTxnMeta(meta));
}

lsmkv::Status Database::Begin(std::unique_ptr<Transaction>* txn) {
    if (txn == nullptr) return STATUS(InvalidArgument, "null txn");
    // Caller must pass an empty unique_ptr; do not silently replace an open txn.
    if (txn->get() != nullptr) {
        return STATUS(InvalidArgument, "txn already holds a transaction");
    }
    std::lock_guard<std::mutex> lock(mu_);
    const TxnId id = next_txn_id_++;
    const Timestamp start_ts = next_ts_ - 1;
    RELDB_RETURN_NOT_OK(PersistOracles());
    TxnMeta meta;
    meta.state = TxnState::kOpen;
    meta.commit_ts = 0;
    RELDB_RETURN_NOT_OK(PutTxnMeta(id, meta));
    *txn = std::unique_ptr<Transaction>(new Transaction(this, id, start_ts));
    return STATUS(OK);
}

lsmkv::Status Database::RestoreHeads(const std::vector<TxnWrite>& writes) {
    for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
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

lsmkv::Status Database::RestoreWrittenHeads(Transaction* txn) {
    std::vector<TxnWrite> writes;
    writes.reserve(txn->written_.size());
    for (const auto& w : txn->written_) {
        writes.push_back(TxnWrite{w.table, w.pk, w.version_id});
    }
    return RestoreHeads(writes);
}

// Idempotent: stamp provisional versions with commit_ts and close prior live versions.
lsmkv::Status Database::ApplyCommitWrites(TxnId txn_id, Timestamp commit_ts,
                                          const std::vector<TxnWrite>& writes) {
    for (const auto& w : writes) {
        VersionRecord rec;
        RELDB_RETURN_NOT_OK(store_->GetVersion(w.table, w.pk, w.version_id, &rec));
        if (rec.created_by != txn_id) {
            return STATUS(Corruption, "apply: version owned by other txn");
        }
        if (!rec.is_provisional()) {
            // Already stamped (redo after partial apply).
            if (rec.start_ts != commit_ts) {
                return STATUS(Corruption, "apply: version stamped with unexpected ts");
            }
            continue;
        }

        if (rec.prev_id != 0) {
            VersionRecord prev;
            auto st = store_->GetVersion(w.table, w.pk, rec.prev_id, &prev);
            if (st.ok() && !prev.is_provisional() && prev.end_ts == 0) {
                prev.end_ts = commit_ts;
                RELDB_RETURN_NOT_OK(store_->PutVersionValue(w.table, w.pk, prev));
            } else if (st.ok() && !prev.is_provisional() && prev.end_ts == commit_ts) {
                // already closed by a previous partial apply
            } else if (!st.ok() && !st.IsNotFound()) {
                return st;
            }
        }

        rec.start_ts = commit_ts;
        RELDB_RETURN_NOT_OK(store_->PutVersionValue(w.table, w.pk, rec));
    }
    return STATUS(OK);
}

lsmkv::Status Database::RecoverTxns() {
    // next_txn_id_ is the next free id; live/historical ids are [1, next_txn_id_).
    for (TxnId id = 1; id < next_txn_id_; ++id) {
        TxnMeta meta;
        auto st = GetTxnMeta(id, &meta);
        if (st.IsNotFound()) continue;
        RELDB_RETURN_NOT_OK(st);

        if (meta.state == TxnState::kCommitting) {
            // Redo apply then mark Committed (Option A).
            if (meta.commit_ts == 0) {
                return STATUS(Corruption, "committing txn missing commit_ts");
            }
            RELDB_RETURN_NOT_OK(ApplyCommitWrites(id, meta.commit_ts, meta.writes));
            TxnMeta done;
            done.state = TxnState::kCommitted;
            done.commit_ts = meta.commit_ts;
            RELDB_RETURN_NOT_OK(PutTxnMeta(id, done));
            // Advance the oracle past any recovered commit_ts.
            if (meta.commit_ts >= next_ts_) {
                next_ts_ = meta.commit_ts + 1;
                RELDB_RETURN_NOT_OK(PersistOracles());
            }
        } else if (meta.state == TxnState::kOpen) {
            // Crash left an open txn: abort and restore heads when write list known.
            // Write list is only durable on Committing today; Open meta usually has
            // empty writes, so restore is a no-op and readers skip Aborted provisionals.
            RELDB_RETURN_NOT_OK(RestoreHeads(meta.writes));
            TxnMeta aborted;
            aborted.state = TxnState::kAborted;
            aborted.commit_ts = 0;
            RELDB_RETURN_NOT_OK(PutTxnMeta(id, aborted));
        }
    }
    return STATUS(OK);
}

lsmkv::Status Database::CommitTransaction(Transaction* txn) {
    std::lock_guard<std::mutex> lock(mu_);
    if (txn->finished_) return STATUS(InvalidArgument, "transaction already finished");

    // First-committer-wins re-check before allocating commit_ts.
    for (const auto& w : txn->written_) {
        Timestamp head = 0;
        RELDB_RETURN_NOT_OK(store_->GetLatestVersionId(w.table, w.pk, &head));
        if (head != w.version_id) {
            // Our provisional is no longer the head (another writer won the race
            // or we lost the key). Cannot commit: restore our provisional heads
            // and mark this txn Aborted so readers ignore our versions.
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
                // A version committed after our snapshot sits under our
                // provisional. SI first-committer-wins: abort rather than
                // overwrite that newer committed write. Restore heads and
                // mark Aborted so our provisionals are no longer visible.
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
    std::vector<TxnWrite> writes;
    writes.reserve(txn->written_.size());
    for (const auto& w : txn->written_) {
        writes.push_back(TxnWrite{w.table, w.pk, w.version_id});
    }

    // Option A prepare: durable Committing intent before any version stamps.
    TxnMeta intent;
    intent.state = TxnState::kCommitting;
    intent.commit_ts = commit_ts;
    intent.writes = writes;
    RELDB_RETURN_NOT_OK(PutTxnMeta(txn->txn_id_, intent));

    // Apply (idempotent — RecoverTxns may re-run this).
    RELDB_RETURN_NOT_OK(ApplyCommitWrites(txn->txn_id_, commit_ts, writes));

    // Finish: never mark Committed while versions still provisional.
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
    if (txn->finished_) return STATUS(OK);
    RELDB_RETURN_NOT_OK(RestoreWrittenHeads(txn));
    TxnMeta meta;
    meta.state = TxnState::kAborted;
    meta.commit_ts = 0;
    RELDB_RETURN_NOT_OK(PutTxnMeta(txn->txn_id_, meta));
    txn->finished_ = true;
    return STATUS(OK);
}

}  // namespace reldb
