#include "reldb/txn.h"

#include <cstring>
#include <mutex>
#include <shared_mutex>

#include "reldb/database.h"
#include "reldb/macros.h"
#include "reldb/schema.h"

namespace reldb {
namespace {

bool HasPrefix(const lsmkv::Slice& key, const std::string& prefix) {
    return key.size() >= prefix.size() &&
           std::memcmp(key.data(), prefix.data(), prefix.size()) == 0;
}

// Head key is "d/<table>/<pk_hex>"; return pk_hex suffix or empty if malformed.
std::string PkHexFromHeadKey(const lsmkv::Slice& key, const std::string& prefix) {
    if (!HasPrefix(key, prefix)) return {};
    return std::string(key.data() + prefix.size(), key.size() - prefix.size());
}

}  // namespace

Transaction::Transaction(std::shared_ptr<Database> db, TxnId txn_id, Timestamp start_ts)
    : db_(std::move(db)), txn_id_(txn_id), start_ts_(start_ts) {}

Transaction::~Transaction() {
    if (!finished_) Abort();
}

lsmkv::Status Transaction::Write(const std::string& table, const Value& pk, bool is_delete,
                                 const Row* row, bool is_insert) {
    if (finished_) return STATUS(InvalidArgument, "transaction finished");

    std::unique_lock<std::shared_mutex> lock(db_->mu_);

    // Already wrote this PK in this txn? Update the same provisional version.
    for (auto& w : written_) {
        if (w.table == table && w.pk == pk) {
            VersionRecord rec;
            RELDB_RETURN_NOT_OK(db_->store_->GetVersion(table, pk, w.version_id, &rec));
            if (is_insert && !rec.is_tombstone) {
                return STATUS(InvalidArgument, "duplicate primary key in transaction");
            }
            if (is_delete) {
                if (rec.is_tombstone) {
                    return STATUS(NotFound, "row already deleted in transaction");
                }
                rec.is_tombstone = true;
                rec.payload.clear();
            } else {
                // Update, or insert-after-delete in the same txn.
                rec.is_tombstone = false;
                rec.payload = row->Encode();
            }
            RELDB_RETURN_NOT_OK(db_->store_->PutVersionValue(table, pk, rec));
            return STATUS(OK);
        }
    }

    Timestamp head_id = 0;
    auto st = db_->store_->GetLatestVersionId(table, pk, &head_id);
    Timestamp prev_id = 0;
    if (st.ok()) {
        prev_id = head_id;
        VersionRecord head;
        RELDB_RETURN_NOT_OK(db_->store_->GetVersion(table, pk, head_id, &head));

        if (head.is_provisional()) {
            TxnMeta meta;
            RELDB_RETURN_NOT_OK(db_->GetTxnMeta(head.created_by, &meta));
            if (meta.state == TxnState::kOpen && head.created_by != txn_id_) {
                return STATUS(Conflict, "write-write conflict: key held by open txn");
            }
            if (meta.state == TxnState::kOpen && head.created_by == txn_id_) {
                // Should have been found in written_ — treat as overwrite.
                return STATUS(Corruption, "missing written_ entry for own provisional");
            }
            // Aborted provisional at head: skip it as prev for chain purposes.
            if (meta.state == TxnState::kAborted) {
                prev_id = head.prev_id;
            }
            // Committed but unstamped: treat as committed live row below via GetRow.
        }

        // First-committer-wins: a committed version newer than our snapshot
        // means another txn already committed a write on this key.
        Timestamp walk = head_id;
        while (walk != 0) {
            VersionRecord cur;
            RELDB_RETURN_NOT_OK(db_->store_->GetVersion(table, pk, walk, &cur));
            if (!cur.is_provisional()) {
                if (cur.start_ts > start_ts_) {
                    return STATUS(Conflict, "write-write conflict: key committed after snapshot");
                }
                break;  // newest committed is enough
            }
            if (cur.created_by != txn_id_) {
                TxnMeta meta;
                RELDB_RETURN_NOT_OK(db_->GetTxnMeta(cur.created_by, &meta));
                if (meta.state == TxnState::kCommitted && meta.commit_ts > start_ts_) {
                    return STATUS(Conflict, "write-write conflict: key committed after snapshot");
                }
            }
            walk = cur.prev_id;
        }

        // Snapshot check for insert vs update/delete semantics.
        Row existing;
        auto g = db_->store_->GetRow(
            table, pk, start_ts_, txn_id_,
            [this](TxnId id, TxnMeta* m) { return db_->GetTxnMeta(id, m); }, &existing);
        if (is_insert) {
            if (g.ok()) {
                return STATUS(InvalidArgument, "duplicate primary key");
            }
            if (!g.IsNotFound()) return g;
        } else {
            if (g.IsNotFound()) {
                return STATUS(NotFound, "row not found");
            }
            RELDB_RETURN_NOT_OK(g);
        }
    } else if (st.IsNotFound()) {
        if (!is_insert) {
            return STATUS(NotFound, "row not found");
        }
    } else {
        return st;
    }

    const Timestamp vid = db_->next_version_id_++;
    RELDB_RETURN_NOT_OK(db_->PersistOracles());

    VersionRecord rec;
    rec.version_id = vid;
    rec.start_ts = 0;  // provisional
    rec.end_ts = 0;
    rec.prev_id = prev_id;
    rec.created_by = txn_id_;
    if (is_delete) {
        rec.is_tombstone = true;
    } else {
        rec.is_tombstone = false;
        rec.payload = row->Encode();
    }
    RELDB_RETURN_NOT_OK(db_->store_->PutVersion(table, pk, rec));

    WrittenKey w;
    w.table = table;
    w.pk = pk;
    w.version_id = vid;
    written_.push_back(std::move(w));

    // Durable write list on Open meta so crash recovery can restore heads.
    TxnMeta meta;
    meta.state = TxnState::kOpen;
    meta.commit_ts = 0;
    meta.writes.reserve(written_.size());
    for (const auto& wk : written_) {
        meta.writes.push_back(TxnWrite{wk.table, wk.pk, wk.version_id});
    }
    RELDB_RETURN_NOT_OK(db_->PutTxnMeta(txn_id_, meta));
    return STATUS(OK);
}

lsmkv::Status Transaction::Insert(const std::string& table, const Row& row) {
    TableSchema schema;
    // Database::GetTable takes mu_; Write takes mu_ again (non-nested).
    RELDB_RETURN_NOT_OK(db_->GetTable(table, &schema));
    RELDB_RETURN_NOT_OK(row.ValidateAgainst(schema));
    Value pk;
    RELDB_RETURN_NOT_OK(row.PrimaryKey(schema, &pk));
    return Write(table, pk, /*is_delete=*/false, &row, /*is_insert=*/true);
}

lsmkv::Status Transaction::Update(const std::string& table, const Row& row) {
    TableSchema schema;
    RELDB_RETURN_NOT_OK(db_->GetTable(table, &schema));
    RELDB_RETURN_NOT_OK(row.ValidateAgainst(schema));
    Value pk;
    RELDB_RETURN_NOT_OK(row.PrimaryKey(schema, &pk));
    return Write(table, pk, /*is_delete=*/false, &row, /*is_insert=*/false);
}

lsmkv::Status Transaction::Delete(const std::string& table, const Value& pk) {
    TableSchema schema;
    // Ensures the table exists under mu_ before Write (which also takes mu_).
    RELDB_RETURN_NOT_OK(db_->GetTable(table, &schema));
    return Write(table, pk, /*is_delete=*/true, /*row=*/nullptr, /*is_insert=*/false);
}

lsmkv::Status Transaction::Get(const std::string& table, const Value& pk, Row* out) {
    if (finished_) return STATUS(InvalidArgument, "transaction finished");
    if (out == nullptr) return STATUS(InvalidArgument, "null out");
    // Catalog existence via GetTable (shared or unique internally — does not nest).
    TableSchema schema;
    RELDB_RETURN_NOT_OK(db_->GetTable(table, &schema));
    (void)schema;  // existence check; row layout comes from stored payload
    // Concurrent snapshot reads share mu_; writers take exclusive in Write/Commit.
    std::shared_lock<std::shared_mutex> lock(db_->mu_);
    return db_->store_->GetRow(
        table, pk, start_ts_, txn_id_,
        [this](TxnId id, TxnMeta* m) { return db_->GetTxnMeta(id, m); }, out);
}

TableRowScan::TableRowScan(std::shared_ptr<Database> db, TxnId txn_id, Timestamp start_ts,
                           std::string table, std::string prefix, std::string end_key,
                           bool has_end, std::unique_ptr<lsmkv::Iterator> it)
    : db_(std::move(db)),
      txn_id_(txn_id),
      start_ts_(start_ts),
      table_(std::move(table)),
      prefix_(std::move(prefix)),
      end_key_(std::move(end_key)),
      has_end_(has_end),
      it_(std::move(it)) {
    status_ = Advance();
}

TableRowScan::~TableRowScan() = default;

void TableRowScan::Next() {
    if (!valid_) return;
    it_->Next();
    status_ = Advance();
}

lsmkv::Status TableRowScan::Advance() {
    valid_ = false;
    pk_ = Value();
    row_ = Row();
    RELDB_RETURN_NOT_OK(status_);

    while (it_->Valid()) {
        RELDB_RETURN_NOT_OK(it_->status());

        const lsmkv::Slice key = it_->key();
        if (!HasPrefix(key, prefix_)) {
            return STATUS(OK);  // left the table prefix
        }
        if (has_end_ && key.compare(lsmkv::Slice(end_key_)) >= 0) {
            return STATUS(OK);  // exclusive end bound
        }

        const std::string pk_hex = PkHexFromHeadKey(key, prefix_);
        Value pk;
        RELDB_RETURN_NOT_OK(DecodePkFromKey(pk_hex, &pk));

        Row row;
        lsmkv::Status st;
        {
            std::shared_lock<std::shared_mutex> lock(db_->mu_);
            st = db_->store_->GetRow(
                table_, pk, start_ts_, txn_id_,
                [this](TxnId id, TxnMeta* m) { return db_->GetTxnMeta(id, m); }, &row);
        }
        if (st.IsNotFound()) {
            it_->Next();
            continue;  // no live version at this snapshot
        }
        RELDB_RETURN_NOT_OK(st);

        pk_ = std::move(pk);
        row_ = std::move(row);
        valid_ = true;
        return STATUS(OK);
    }
    return it_->status();
}

lsmkv::Status Transaction::Scan(const std::string& table, const Value* start_pk,
                                const Value* end_pk, std::unique_ptr<TableRowScan>* out) {
    if (finished_) return STATUS(InvalidArgument, "transaction finished");
    if (out == nullptr) return STATUS(InvalidArgument, "null out");
    if (out->get() != nullptr) {
        return STATUS(InvalidArgument, "out already holds a scan");
    }
    TableSchema schema;
    RELDB_RETURN_NOT_OK(db_->GetTable(table, &schema));
    (void)schema;  // existence check; scan uses table name for key prefix

    const std::string prefix = "d/" + table + "/";
    std::string seek_target = prefix;
    if (start_pk != nullptr) {
        seek_target = RowHeadKey(table, EncodePkForKey(*start_pk));
    }
    std::string end_key;
    const bool has_end = (end_pk != nullptr);
    if (has_end) {
        end_key = RowHeadKey(table, EncodePkForKey(*end_pk));
    }

    auto it = db_->kv()->NewIterator(lsmkv::ReadOptions());
    it->Seek(seek_target);

    *out = std::unique_ptr<TableRowScan>(
        new TableRowScan(db_, txn_id_, start_ts_, table, prefix, std::move(end_key), has_end,
                         std::move(it)));
    if (!(*out)->status().ok()) {
        return (*out)->status();
    }
    return STATUS(OK);
}

lsmkv::Status Transaction::Commit() {
    if (finished_) return STATUS(InvalidArgument, "transaction finished");
    return db_->CommitTransaction(this);
}

lsmkv::Status Transaction::Abort() {
    if (finished_) return STATUS(OK);
    return db_->AbortTransaction(this);
}

void Transaction::TEST_AbandonWithoutAbort() {
    // Keep open_txn_count_ accurate for the DDL gate: abandon still finishes the
    // live txn from the Database's perspective without a durable Abort.
    if (db_ != nullptr && !finished_) {
        std::unique_lock<std::shared_mutex> lock(db_->mu_);
        db_->MarkTxnFinishedLocked(this);
    } else {
        finished_ = true;
    }
    db_.reset();
}

}  // namespace reldb
