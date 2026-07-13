#pragma once

// SQL server: pure request helpers (A2) + TCP front-end (A3).
// SqlHandleLine has no sockets and is unit-tested directly.
// SqlServer owns Database, accept loop, and per-connection sessions.

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "lsmkv/status.h"
#include "reldb/database.h"
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
// connection teardown (client drop).
// Returns the Abort status if a txn was open; OK if none.
lsmkv::Status SqlResetSession(SqlSession& session, std::string& conn_buffer);

struct SqlServerConfig {
    std::string host = "127.0.0.1";  // localhost default (no auth)
    int port = 7380;                 // 0 = ephemeral (tests)
    std::string db_path = "./reldb_sql_data";
    int max_clients = 128;
};

// TCP front-end for reldb::Database + SqlSession per connection.
// Mirrors the structure of lsmkv::Server (Start / Serve / Stop).
class SqlServer {
public:
    explicit SqlServer(SqlServerConfig config);
    ~SqlServer();

    SqlServer(const SqlServer&) = delete;
    SqlServer& operator=(const SqlServer&) = delete;

    // Open Database and bind/listen. Safe to call once.
    lsmkv::Status Start();

    // Blocking accept loop until Stop() (or fatal accept error).
    // Returns InvalidArgument if called before a successful Start().
    lsmkv::Status Serve();

    // Thread-safe request to leave Serve().
    void Stop();

    // Bound TCP port after a successful Start() (resolved if config.port was 0).
    int port() const { return bound_port_; }

    int active_clients() const {
        return active_clients_.load(std::memory_order_acquire);
    }

    std::shared_ptr<Database> database() const { return db_; }

private:
    bool TryReserveClientSlot();
    void ReleaseClientSlot();
    void ServeClient(int client_fd);

    SqlServerConfig config_;
    std::shared_ptr<Database> db_;
    int listen_fd_ = -1;
    int bound_port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> active_clients_{0};
};

bool ParseSqlServerArgs(int argc, char** argv, SqlServerConfig* cfg, std::string* error);
void PrintSqlServerUsage(const char* argv0);

}  // namespace reldb
