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
      catalog_(std::make_shared<Catalog>(kv_)),
      store_(std::make_shared<MvccStore>(kv_)) {}

Database::~Database() = default;

lsmkv::Status Database::Open(const lsmkv::Options& options, const std::string& path,
                             Database** dbptr) {
    if (dbptr == nullptr) {
        return STATUS(InvalidArgument, "null dbptr");
    }
    lsmkv::DB* raw = nullptr;
    RELDB_RETURN_NOT_OK(lsmkv::DB::Open(options, path, &raw));
    auto* db = new Database(std::shared_ptr<lsmkv::DB>(raw));
    auto st = db->InitOracles();
    if (!st.ok()) {
        delete db;
        return st;
    }
    *dbptr = db;
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

lsmkv::Status Database::Begin(Transaction** txn) {
    if (txn == nullptr) return STATUS(InvalidArgument, "null txn");
    std::lock_guard<std::mutex> lock(mu_);
    const TxnId id = next_txn_id_++;
    const Timestamp start_ts = next_ts_ - 1;
    RELDB_RETURN_NOT_OK(PersistOracles());
    TxnMeta meta;
    meta.state = TxnState::kOpen;
    RELDB_RETURN_NOT_OK(PutTxnMeta(id, meta));
    *txn = new Transaction(this, id, start_ts);
    return STATUS(OK);
}

lsmkv::Status Database::CommitTransaction(Transaction* txn) {
    std::lock_guard<std::mutex> lock(mu_);
    if (txn->finished_) return STATUS(InvalidArgument, "transaction already finished");
    // No writes in this PR — just mark committed and advance the clock.
    const Timestamp commit_ts = next_ts_++;
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
    TxnMeta meta;
    meta.state = TxnState::kAborted;
    RELDB_RETURN_NOT_OK(PutTxnMeta(txn->txn_id_, meta));
    txn->finished_ = true;
    return STATUS(OK);
}

}  // namespace reldb
