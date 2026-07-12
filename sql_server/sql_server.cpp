#include "sql_server.h"

#include <cctype>

#include "protocol.h"
#include "reldb/macros.h"
#include "reldb/query_result.h"

namespace reldb {
namespace {

std::string_view TrimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// Uppercase ASCII copy for meta-command matching.
std::string ToUpperAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        out.push_back(static_cast<char>(std::toupper(ch)));
    }
    return out;
}

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

lsmkv::Status SqlResetSession(SqlSession* session, std::string* conn_buffer) {
    if (conn_buffer != nullptr) {
        conn_buffer->clear();
    }
    if (session == nullptr || !session->InTransaction()) {
        return STATUS(OK);
    }
    QueryResult unused;
    // ABORT via SQL keeps a single path through SqlSession.
    return session->Execute("ABORT;", unused);
}

bool SqlHandleLine(SqlSession* session, std::string* conn_buffer, std::string_view line,
                   std::string* reply) {
    if (reply == nullptr) {
        return true;
    }
    reply->clear();
    if (session == nullptr || conn_buffer == nullptr) {
        *reply = sqlproto::EncodeError(lsmkv::Status::InvalidArgument("null session or buffer"));
        return true;
    }

    // Meta commands only when not mid-statement.
    if (conn_buffer->empty()) {
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
                *reply = sqlproto::EncodeStatus(session->InTransaction());
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
    if (!sqlproto::TryAppendLine(conn_buffer, line, &err)) {
        // Cap exceeded: drop partial statement, keep connection.
        conn_buffer->clear();
        *reply = std::move(err);
        return true;
    }

    if (!sqlproto::EndsWithStatementTerminator(*conn_buffer)) {
        // Incomplete multi-line statement — wait for more lines (empty reply).
        return true;
    }

    QueryResult result;
    const auto st = session->Execute(*conn_buffer, result);
    conn_buffer->clear();
    if (!st.ok()) {
        *reply = sqlproto::EncodeError(st);
    } else {
        *reply = sqlproto::EncodeQueryResult(result);
    }
    return true;
}

}  // namespace reldb
