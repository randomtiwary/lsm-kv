#include "sql_server.h"

#include "protocol.h"
#include "reldb/macros.h"
#include "reldb/query_result.h"
#include "reldb/string_util.h"

namespace reldb {
namespace {

// Meta commands only apply when the statement buffer is empty (fresh line).
// Matched case-insensitively against the full trimmed line.
enum class MetaCmd { kNone, kPing, kQuit, kReset, kStatus };

MetaCmd ParseMeta(std::string_view trimmed) {
    if (trimmed.empty()) return MetaCmd::kNone;
    const std::string u = ToUpperAscii(trimmed);
    if (u == "PING") return MetaCmd::kPing;
    if (u == "QUIT") return MetaCmd::kQuit;
    if (u == "RESET") return MetaCmd::kReset;
    if (u == "STATUS") return MetaCmd::kStatus;
    return MetaCmd::kNone;
}

}  // namespace

lsmkv::Status SqlResetSession(SqlSession& session, std::string& conn_buffer) {
    conn_buffer.clear();
    if (!session.InTransaction()) {
        return STATUS(OK);
    }
    QueryResult unused;
    // ABORT via SQL keeps a single path through SqlSession.
    return session.Execute("ABORT;", unused);
}

bool SqlHandleLine(SqlSession& session, std::string& conn_buffer, std::string_view line,
                   std::string* reply) {
    if (reply == nullptr) {
        return true;
    }
    reply->clear();

    // Meta commands only when not mid-statement.
    if (conn_buffer.empty()) {
        const std::string_view trimmed = TrimView(line);
        switch (ParseMeta(trimmed)) {
            case MetaCmd::kPing:
                *reply = sqlproto::EncodePong();
                return true;
            case MetaCmd::kQuit:
                *reply = sqlproto::EncodeOk();
                return false;  // close after reply
            case MetaCmd::kReset: {
                auto st = SqlResetSession(session, conn_buffer);
                if (!st.ok()) {
                    *reply = sqlproto::EncodeError(st);
                } else {
                    *reply = sqlproto::EncodeOk();
                }
                return true;
            }
            case MetaCmd::kStatus:
                *reply = sqlproto::EncodeStatus(session.InTransaction());
                return true;
            case MetaCmd::kNone:
                break;
        }
        // Empty line with empty buffer: no-op (no reply).
        if (trimmed.empty()) {
            return true;
        }
    }

    std::string err;
    if (!sqlproto::TryAppendLine(&conn_buffer, line, &err)) {
        // Cap exceeded: drop partial statement, keep connection.
        conn_buffer.clear();
        *reply = std::move(err);
        return true;
    }

    if (!sqlproto::EndsWithStatementTerminator(conn_buffer)) {
        // Incomplete multi-line statement — wait for more lines (empty reply).
        return true;
    }

    QueryResult result;
    const auto st = session.Execute(conn_buffer, result);
    conn_buffer.clear();
    if (!st.ok()) {
        *reply = sqlproto::EncodeError(st);
    } else {
        *reply = sqlproto::EncodeQueryResult(result);
    }
    return true;
}

}  // namespace reldb
