#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "lsmkv/db.h"
#include "lsmkv/status.h"
#include "reldb/common.h"
#include "reldb/row.h"
#include "reldb/types.h"

namespace reldb {

// One physical version of a row in the MVCC chain.
//
// Lifetime of this version is the half-open interval [start_ts, end_ts):
//   - start_ts: commit timestamp that created this version
//   - end_ts:   commit timestamp that superseded it; 0 means still live
//               (treat as +infinity)
//   - prev_ts:  start_ts of the previous version for this PK (0 = none)
//
// A snapshot S sees this version iff start_ts <= S < end_ts
// (with end_ts == 0 meaning no upper bound). At most one version of a row
// is visible at a given S because lifetimes form a non-overlapping chain.
struct VersionRecord {
    Timestamp start_ts = 0;
    Timestamp end_ts = 0;   // 0 means still live (infinity)
    Timestamp prev_ts = 0;  // 0 means no older version
    bool is_tombstone = false;
    std::string payload;  // Row::Encode() bytes when !is_tombstone

    std::string Encode() const;
    static lsmkv::Status Decode(const std::string& bytes, VersionRecord* out);
};

// Snapshot isolation visibility (see VersionRecord comment above).
// Equivalent to: start_ts <= snapshot && (end_ts == 0 || end_ts > snapshot).
bool IsVisible(const VersionRecord& v, Timestamp snapshot);

// Hex of Row::EncodeValue(pk) so keys stay free of raw binary / separators.
std::string EncodePkForKey(const Value& pk);
lsmkv::Status DecodePkFromKey(const std::string& hex, Value* out);

// Physical key helpers (also used by tests).
std::string RowHeadKey(const std::string& table, const std::string& pk_key);
std::string VersionKey(const std::string& table, const std::string& pk_key,
                       Timestamp start_ts);

// MVCC point-read / version-write helpers on top of lsmkv::DB.
// No transaction logic here — only version chains and visibility.
// Shares ownership of the underlying DB via shared_ptr (same pattern as Catalog).
class MvccStore {
public:
    explicit MvccStore(std::shared_ptr<lsmkv::DB> db);

    // Walk the version chain from the row head; return the unique visible
    // non-tombstone row at `snapshot`, or NotFound.
    lsmkv::Status GetRow(const std::string& table, const Value& pk,
                         Timestamp snapshot, Row* out) const;

    // Latest version's start_ts for a PK, or NotFound if the row never existed.
    // Used by the transaction layer for write-write conflict checks.
    lsmkv::Status GetLatestStartTs(const std::string& table, const Value& pk,
                                   Timestamp* out_ts) const;

    // Load a single version by start_ts.
    lsmkv::Status GetVersion(const std::string& table, const Value& pk,
                             Timestamp start_ts, VersionRecord* out) const;

    // Install a new version at start_ts and update the row head pointer.
    // Does not close the previous version; caller sets end_ts on the old one
    // via CloseVersion when superseding.
    lsmkv::Status PutVersion(const std::string& table, const Value& pk,
                             const VersionRecord& rec);

    // Rewrite an existing version with a new end_ts (supersede / delete).
    lsmkv::Status CloseVersion(const std::string& table, const Value& pk,
                               Timestamp start_ts, Timestamp end_ts);

private:
    std::shared_ptr<lsmkv::DB> db_;
};

}  // namespace reldb
