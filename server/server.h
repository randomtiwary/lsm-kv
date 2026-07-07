#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "lsmkv/db.h"
#include "lsmkv/status.h"

namespace lsmkv {

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 7379;              // 0 = ephemeral (useful in tests)
    std::string db_path = "./lsmkv_data";
    int max_clients = 128;
};

// Execute one protocol request against `db`.
// On success, `reply` holds the full bytes to send (line(s) with trailing '\n').
// Returns false if the connection should be closed after sending `reply` (QUIT).
// Pure protocol helper — no sockets — so unit tests can call it directly.
bool ExecuteRequest(DB* db, const std::string& line, std::string* reply);

// TCP front-end for an embedded DB. State (running flag, active client count,
// listen socket, DB ownership) lives on the instance — not in globals.
class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Open the DB and bind/listen. Safe to call once.
    Status Start();

    // Blocking accept loop until Stop() is called (or a fatal accept error).
    void Serve();

    // Thread-safe request to leave Serve().
    void Stop();

    // Bound TCP port after a successful Start() (resolved if config.port was 0).
    int port() const { return bound_port_; }

    int active_clients() const {
        return active_clients_.load(std::memory_order_acquire);
    }

private:
    bool TryReserveClientSlot();
    void ReleaseClientSlot();
    void ServeClient(int client_fd);

    ServerConfig config_;
    std::unique_ptr<DB> db_;
    int listen_fd_ = -1;
    int bound_port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> active_clients_{0};
};

// CLI helpers shared with main.
bool ParseServerArgs(int argc, char** argv, ServerConfig* cfg, std::string* error);
void PrintServerUsage(const char* argv0);

}  // namespace lsmkv
