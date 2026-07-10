#include "reldb/mvcc.h"

#include "lsmkv/encoding.h"
#include "lsmkv/options.h"
#include "lsmkv/slice.h"
#include "reldb/macros.h"

namespace reldb {
namespace {

std::string ToHex(const std::string& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        auto b = static_cast<unsigned char>(bytes[i]);
        out[2 * i] = kHex[b >> 4];
        out[2 * i + 1] = kHex[b & 0xf];
    }
    return out;
}

bool FromHex(const std::string& hex, std::string* out) {
    if (hex.size() % 2 != 0) return false;
    out->clear();
    out->reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

void AppendFixed64BE(std::string* dst, Timestamp v) {
    char buf[8];
    for (int i = 7; i >= 0; --i) {
        buf[i] = static_cast<char>(v & 0xff);
        v >>= 8;
    }
    dst->append(buf, 8);
}

std::string Fixed64BE(Timestamp v) {
    std::string s;
    AppendFixed64BE(&s, v);
    return s;
}

}  // namespace

std::string VersionRecord::Encode() const {
    std::string out;
    lsmkv::PutFixed64(&out, version_id);
    lsmkv::PutFixed64(&out, start_ts);
    lsmkv::PutFixed64(&out, end_ts);
    lsmkv::PutFixed64(&out, prev_id);
    lsmkv::PutFixed64(&out, created_by);
    out.push_back(is_tombstone ? '\x01' : '\x00');
    lsmkv::PutLengthPrefixedSlice(&out, payload);
    return out;
}

lsmkv::Status VersionRecord::Decode(const std::string& bytes, VersionRecord* out) {
    if (out == nullptr) {
        return STATUS(InvalidArgument, "null out");
    }
    lsmkv::Slice input(bytes);
    VersionRecord rec;
    if (!lsmkv::GetFixed64(&input, &rec.version_id) ||
        !lsmkv::GetFixed64(&input, &rec.start_ts) ||
        !lsmkv::GetFixed64(&input, &rec.end_ts) ||
        !lsmkv::GetFixed64(&input, &rec.prev_id) ||
        !lsmkv::GetFixed64(&input, &rec.created_by)) {
        return STATUS(Corruption, "version: truncated header");
    }
    if (input.empty()) {
        return STATUS(Corruption, "version: missing flags");
    }
    rec.is_tombstone = (input.data()[0] != 0);
    input.remove_prefix(1);
    lsmkv::Slice payload;
    if (!lsmkv::GetLengthPrefixedSlice(&input, &payload)) {
        return STATUS(Corruption, "version: bad payload");
    }
    if (!input.empty()) {
        return STATUS(Corruption, "version: trailing bytes");
    }
    rec.payload = payload.ToString();
    *out = std::move(rec);
    return STATUS(OK);
}

bool IsCommittedVisible(const VersionRecord& v, Timestamp snapshot) {
    if (v.is_provisional()) return false;
    if (v.start_ts > snapshot) return false;
    if (v.end_ts != 0 && v.end_ts <= snapshot) return false;
    return true;
}

std::string EncodePkForKey(const Value& pk) {
    return ToHex(Row::EncodeValue(pk));
}

lsmkv::Status DecodePkFromKey(const std::string& hex, Value* out) {
    std::string raw;
    if (!FromHex(hex, &raw)) {
        return STATUS(Corruption, "bad pk hex");
    }
    return Row::DecodeValue(raw, out);
}

std::string RowHeadKey(const std::string& table, const std::string& pk_key) {
    return "d/" + table + "/" + pk_key;
}

std::string VersionKey(const std::string& table, const std::string& pk_key,
                       Timestamp version_id) {
    return "v/" + table + "/" + pk_key + "/" + Fixed64BE(version_id);
}

std::string TxnKey(TxnId id) {
    return "m/txn/" + Fixed64BE(id);
}

MvccStore::MvccStore(std::shared_ptr<lsmkv::DB> db) : db_(std::move(db)) {}

lsmkv::Status MvccStore::GetVersion(const std::string& table, const Value& pk,
                                    Timestamp version_id,
                                    VersionRecord* out) const {
    const std::string pk_key = EncodePkForKey(pk);
    std::string bytes;
    RELDB_RETURN_NOT_OK(
        db_->Get(lsmkv::ReadOptions(), VersionKey(table, pk_key, version_id), &bytes));
    return VersionRecord::Decode(bytes, out);
}

lsmkv::Status MvccStore::GetLatestVersionId(const std::string& table, const Value& pk,
                                            Timestamp* out_id) const {
    const std::string pk_key = EncodePkForKey(pk);
    std::string bytes;
    RELDB_RETURN_NOT_OK(db_->Get(lsmkv::ReadOptions(), RowHeadKey(table, pk_key), &bytes));
    if (bytes.size() != 8) {
        return STATUS(Corruption, "row head: bad length");
    }
    *out_id = lsmkv::DecodeFixed64(bytes.data());
    return STATUS(OK);
}

lsmkv::Status MvccStore::GetRow(const std::string& table, const Value& pk,
                                Timestamp snapshot, TxnId reader_txn,
                                const TxnStatusFn& txn_status, Row* out) const {
    if (out == nullptr) {
        return STATUS(InvalidArgument, "null out");
    }
    Timestamp id = 0;
    auto st = GetLatestVersionId(table, pk, &id);
    if (st.IsNotFound()) {
        return STATUS(NotFound, "row not found");
    }
    RELDB_RETURN_NOT_OK(st);

    while (id != 0) {
        VersionRecord rec;
        RELDB_RETURN_NOT_OK(GetVersion(table, pk, id, &rec));

        bool visible = false;
        if (rec.is_provisional()) {
            if (rec.created_by == reader_txn) {
                visible = true;  // read-your-writes
            } else if (txn_status) {
                TxnMeta meta;
                RELDB_RETURN_NOT_OK(txn_status(rec.created_by, &meta));
                // Commit must stamp start_ts *before* marking the txn Committed,
                // so we should never observe provisional + Committed. If we do,
                // the store is inconsistent — fail loud rather than guess.
                if (meta.state == TxnState::kCommitted) {
                    return STATUS(Corruption,
                                  "provisional version owned by committed txn");
                }
                // Open (other) or Aborted => not visible to this reader
            }
        } else {
            visible = IsCommittedVisible(rec, snapshot);
        }

        if (visible) {
            if (rec.is_tombstone) {
                return STATUS(NotFound, "row deleted at snapshot");
            }
            return Row::Decode(rec.payload, out);
        }
        id = rec.prev_id;
    }
    return STATUS(NotFound, "no visible version");
}

lsmkv::Status MvccStore::GetRowCommitted(const std::string& table, const Value& pk,
                                         Timestamp snapshot, Row* out) const {
    return GetRow(table, pk, snapshot, /*reader_txn=*/0, /*txn_status=*/nullptr, out);
}

lsmkv::Status MvccStore::PutVersion(const std::string& table, const Value& pk,
                                    const VersionRecord& rec) {
    RELDB_RETURN_NOT_OK(PutVersionValue(table, pk, rec));
    return SetHead(table, pk, rec.version_id);
}

lsmkv::Status MvccStore::PutVersionValue(const std::string& table, const Value& pk,
                                         const VersionRecord& rec) {
    const std::string pk_key = EncodePkForKey(pk);
    return db_->Put(lsmkv::WriteOptions(), VersionKey(table, pk_key, rec.version_id),
                    rec.Encode());
}

lsmkv::Status MvccStore::SetHead(const std::string& table, const Value& pk,
                                 Timestamp version_id) {
    const std::string pk_key = EncodePkForKey(pk);
    std::string head;
    lsmkv::PutFixed64(&head, version_id);
    return db_->Put(lsmkv::WriteOptions(), RowHeadKey(table, pk_key), head);
}

lsmkv::Status MvccStore::ClearHead(const std::string& table, const Value& pk) {
    const std::string pk_key = EncodePkForKey(pk);
    return db_->Delete(lsmkv::WriteOptions(), RowHeadKey(table, pk_key));
}

}  // namespace reldb
