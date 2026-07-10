#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "lsmkv/common.h"
#include "lsmkv/db.h"
#include "lsmkv/status.h"
#include "reldb/row.h"
#include "reldb/types.h"

namespace reldb {

using Timestamp = lsmkv::Timestamp;
using TxnId = lsmkv::Timestamp;

// Transaction registry record (key m/txn/<id>).
enum class TxnState : std::uint8_t {
    kOpen = 1,
    kCommitted = 2,
    kAborted = 3,
};

struct TxnMeta {
    TxnState state = TxnState::kOpen;
    Timestamp commit_ts = 0;  // valid when state == kCommitted
};

// One physical version of a row in the MVCC chain.
//
// version_id is the durable key identity (unique per version install).
// For a committed version, start_ts is the commit timestamp.
// For a provisional (in-flight) version, start_ts == 0 and created_by is the
// open transaction that owns it.
//
// Lifetime once committed: half-open [start_ts, end_ts) with end_ts==0 => live.
struct VersionRecord {
    Timestamp version_id = 0;
    Timestamp start_ts = 0;   // 0 => provisional (not yet committed)
    Timestamp end_ts = 0;     // 0 => still live (infinity) once committed
    Timestamp prev_id = 0;    // previous version_id; 0 = none
    TxnId created_by = 0;     // transaction that installed this version
    bool is_tombstone = false;
    std::string payload;  // Row::Encode() bytes when !is_tombstone

    bool is_provisional() const { return start_ts == 0; }

    std::string Encode() const;
    static lsmkv::Status Decode(const std::string& bytes, VersionRecord* out);
};

// Committed-version visibility: start_ts <= snapshot && (end_ts == 0 || end_ts > snapshot).
bool IsCommittedVisible(const VersionRecord& v, Timestamp snapshot);

// Lookup txn status (used for provisional visibility / WW checks).
using TxnStatusFn = std::function<lsmkv::Status(TxnId id, TxnMeta* out)>;

std::string EncodePkForKey(const Value& pk);
lsmkv::Status DecodePkFromKey(const std::string& hex, Value* out);

std::string RowHeadKey(const std::string& table, const std::string& pk_key);
std::string VersionKey(const std::string& table, const std::string& pk_key,
                       Timestamp version_id);
std::string TxnKey(TxnId id);

class MvccStore {
public:
    explicit MvccStore(std::shared_ptr<lsmkv::DB> db);

    // Point read under snapshot isolation, including the reader's own
    // provisional versions (read-your-writes). Other txns' provisional
    // versions are invisible; committed versions use IsCommittedVisible.
    lsmkv::Status GetRow(const std::string& table, const Value& pk,
                         Timestamp snapshot, TxnId reader_txn,
                         const TxnStatusFn& txn_status, Row* out) const;

    // Convenience for tests that only deal with committed data (reader_txn=0).
    lsmkv::Status GetRowCommitted(const std::string& table, const Value& pk,
                                  Timestamp snapshot, Row* out) const;

    lsmkv::Status GetLatestVersionId(const std::string& table, const Value& pk,
                                     Timestamp* out_id) const;

    lsmkv::Status GetVersion(const std::string& table, const Value& pk,
                             Timestamp version_id, VersionRecord* out) const;

    // Install version (keyed by rec.version_id) and update the row head.
    lsmkv::Status PutVersion(const std::string& table, const Value& pk,
                             const VersionRecord& rec);

    // Rewrite an existing version value (same version_id); does not change head.
    lsmkv::Status PutVersionValue(const std::string& table, const Value& pk,
                                  const VersionRecord& rec);

    lsmkv::Status SetHead(const std::string& table, const Value& pk,
                          Timestamp version_id);

    lsmkv::Status ClearHead(const std::string& table, const Value& pk);

private:
    std::shared_ptr<lsmkv::DB> db_;
};

}  // namespace reldb
