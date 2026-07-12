#pragma once

// SQL wire protocol codec (roadmap A1 / K22).
// Shared by reldb_sql_server, reldb_sql_cli, and tests. No sockets.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lsmkv/status.h"
#include "reldb/query_result.h"
#include "reldb/types.h"

namespace reldb {
namespace sqlproto {

// Max bytes in a multi-line statement buffer before the server rejects.
inline constexpr std::size_t kMaxStatementBytes = 1024 * 1024;

// ---------------------------------------------------------------------------
// Cell codec: one Value <-> one protocol line ($N / $I: / $B: / $S:…)
// ---------------------------------------------------------------------------

// Encode a cell as a single line body (no trailing newline).
std::string EncodeCell(const Value& v);

// Decode one cell line (no trailing newline). On failure returns InvalidArgument.
lsmkv::Status DecodeCell(std::string_view line, Value* out);

// ---------------------------------------------------------------------------
// Reply encoding (always includes trailing '\n' on the last line)
// ---------------------------------------------------------------------------

// "-ERR " + status.ToString() + "\n"
std::string EncodeError(const lsmkv::Status& st);

std::string EncodeOk();                              // +OK\n
std::string EncodeOkRowsAffected(std::uint64_t n);   // +OK rows_affected=<n>\n
std::string EncodePong();                            // +PONG\n
std::string EncodeStatus(bool in_txn);               // +OK in_txn=0|1\n

// Success path from QueryResult (not errors):
//   - has column_names or rows → SELECT result block (*PLAN optional, *RESULT…*END)
//   - else rows_affected > 0 → +OK rows_affected=<n>
//   - else +OK
std::string EncodeQueryResult(const QueryResult& result);

// ---------------------------------------------------------------------------
// Framing helpers (statement buffer)
// ---------------------------------------------------------------------------

// True if s ends a complete statement: a ';' not inside a single-quoted string
// (SQL '' escape), with only trailing whitespace after that ';'.
// Same rules as reldb_sql_shell.
bool EndsWithStatementTerminator(std::string_view s);

// Append a received TCP/client line to *buffer, inserting '\n' between lines.
// If the append would exceed kMaxStatementBytes: does not modify *buffer;
// sets *err_reply to EncodeError(InvalidArgument: statement too large) and
// returns false. On success returns true (err_reply uncleared).
bool TryAppendLine(std::string* buffer, std::string_view line, std::string* err_reply);

// ---------------------------------------------------------------------------
// Reply decoding (CLI / tests)
// ---------------------------------------------------------------------------

enum class ReplyKind {
    kOk,             // +OK or +OK rows_affected= / +OK in_txn=
    kPong,           // +PONG
    kError,          // -ERR …
    kResult,         // *RESULT … *END
    kUnknown,
};

struct DecodedReply {
    ReplyKind kind = ReplyKind::kUnknown;
    std::string error_text;          // body after "-ERR " (no newline)
    std::uint64_t rows_affected = 0;
    bool has_rows_affected = false;
    bool in_txn = false;
    bool has_in_txn = false;
    std::string plan_tag;
    QueryResult result;              // filled for kResult
};

// Decode a full reply blob (may be multi-line). Fails on malformed result blocks
// or bad cell lines.
lsmkv::Status DecodeReply(std::string_view text, DecodedReply* out);

}  // namespace sqlproto
}  // namespace reldb
