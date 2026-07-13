#pragma once

// Per-connection SQL request handling (roadmap A2).
// Pure helpers — no sockets — so unit tests call SqlHandleLine directly.
// TCP front-end (SqlServer) lands in A3.

#include <string>
#include <string_view>

#include "lsmkv/status.h"
#include "reldb/sql_session.h"

namespace reldb {

// Process one received line (without trailing '\n') for a connection.
//
// session       — per-connection SqlSession (transaction stickiness)
// conn_buffer   — multi-line statement buffer for this connection (in/out)
// line          — one line from the client
// *reply        — out: bytes to send (may be empty if statement incomplete)
//
// Returns false if the connection should close after sending *reply (QUIT).
// On statement-too-large, clears conn_buffer, sets an -ERR reply, keeps open.
bool SqlHandleLine(SqlSession& session, std::string& conn_buffer, std::string_view line,
                   std::string* reply);

// Abort any open transaction and clear conn_buffer. Used by RESET and by
// connection teardown helpers in tests.
// Returns the Abort status if a txn was open; OK if none.
lsmkv::Status SqlResetSession(SqlSession& session, std::string& conn_buffer);

}  // namespace reldb
