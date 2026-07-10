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

// One physical version of a row in the MVCC chain (value of v/<table>/<pk>/<version_id>).
struct VersionRecord {
    // Unique durable identity of this version; used as the KV key suffix and as
    // the value stored in the row head (d/<table>/<pk>). Never reused. Assigned
    // when the version is first installed (including provisional installs).
    // Unchanged across provisional → committed transition.
    Timestamp version_id = 0;

    // Commit timestamp that created this version's lifetime.
    //   Uncommitted (provisional): always 0. is_provisional() is true.
    //   Committed: set to the txn's commit_ts when the writer commits (must be
    //              stamped *before* the txn registry is marked Committed).
    Timestamp start_ts = 0;

    // Commit timestamp that superseded this version (update/delete by a later txn).
    //   Uncommitted: always 0.
    //   Committed and still live: 0 meaning +infinity (no upper bound yet).
    //   Committed and superseded: end_ts of the commit that replaced it.
    // Lifetime once committed is the half-open interval [start_ts, end_ts)
    // with end_ts==0 meaning still live.
    Timestamp end_ts = 0;

    // version_id of the previous version on this PK chain (0 = none).
    // Set when this version is installed; links the chain for readers walking
    // from the head toward older versions. Unchanged at commit.
    Timestamp prev_id = 0;

    // Transaction that installed this version.
    //   Uncommitted: the open txn_id that owns the provisional write.
    //   Committed: still that same txn_id (historical; visibility uses start_ts).
    TxnId created_by = 0;

    // If true, this version represents a deletion (no payload).
    //   Uncommitted: may be set by a provisional Delete.
    //   Committed: fixed at commit; tombstone remains for older snapshots.
    bool is_tombstone = false;

    // Row::Encode() bytes when !is_tombstone; empty when tombstone.
    // Written on install / overwrite; unchanged at commit except via rewrite.
    std::string payload;

    // True iff this version has not been committed yet (start_ts not stamped).
    bool is_provisional() const { return start_ts == 0; }

    std::string Encode() const;
    static lsmkv::Status Decode(const std::string& bytes, VersionRecord* out);
};

// Committed-version visibility: start_ts <= snapshot && (end_ts == 0 || end_ts > snapshot).
// Must not be called for provisional records (start_ts == 0).
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
    //
    // Invariant: a Committed txn must not leave versions with start_ts==0.
    // Commit protocols stamp start_ts before marking the txn Committed.
    // Seeing provisional + Committed is Corruption.
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
