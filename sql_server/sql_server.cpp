#include "sql_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <system_error>
#include <thread>

#include "protocol.h"
#include "reldb/macros.h"
#include "reldb/query_result.h"
#include "reldb/string_util.h"

namespace reldb {
namespace {

// ---------------------------------------------------------------------------
// Meta / request handling (no sockets)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// TCP helpers (same shape as lsmkv server; kept local for isolation)
// ---------------------------------------------------------------------------

bool SendAll(int fd, const char* data, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        const ssize_t r = ::send(fd, data + sent, n - sent, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

bool SendLine(int fd, const std::string& line) {
    std::string out = line;
    out.push_back('\n');
    return SendAll(fd, out.data(), out.size());
}

// Read one '\n'-terminated line (strips optional trailing '\r'). Returns false on EOF/error.
bool RecvLine(int fd, std::string* out) {
    out->clear();
    char ch;
    while (true) {
        const ssize_t r = ::recv(fd, &ch, 1, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        if (ch == '\n') break;
        if (ch != '\r') out->push_back(ch);
        if (out->size() > 1024 * 1024) {
            return false;
        }
    }
    return true;
}

}  // namespace

lsmkv::Status SqlResetSession(SqlSession& session, std::string& conn_buffer) {
    conn_buffer.clear();
    if (!session.InTransaction()) {
        return STATUS(OK);
    }
    QueryResult unused;
    return session.Execute("ABORT;", unused);
}

bool SqlHandleLine(SqlSession& session, std::string& conn_buffer, std::string_view line,
                   std::string* reply) {
    if (reply == nullptr) {
        return true;
    }
    reply->clear();

    if (conn_buffer.empty()) {
        const std::string_view trimmed = TrimView(line);
        switch (ParseMeta(trimmed)) {
            case MetaCmd::kPing:
                *reply = sqlproto::EncodePong();
                return true;
            case MetaCmd::kQuit:
                *reply = sqlproto::EncodeOk();
                return false;
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
        if (trimmed.empty()) {
            return true;
        }
    }

    std::string err;
    if (!sqlproto::TryAppendLine(&conn_buffer, line, &err)) {
        conn_buffer.clear();
        *reply = std::move(err);
        return true;
    }

    if (!sqlproto::EndsWithStatementTerminator(conn_buffer)) {
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

// ---------------------------------------------------------------------------
// SqlServer TCP
// ---------------------------------------------------------------------------

namespace {

// Owns a socket fd; closes on destroy unless release() was called.
class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd() { reset(); }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

    int release() {
        const int t = fd_;
        fd_ = -1;
        return t;
    }

private:
    int fd_;
};

// Wait until active_clients hits 0 or timeout; warn if clients remain.
void WaitForIdleClients(const std::atomic<int>& active_clients, std::chrono::seconds timeout,
                        const char* context) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (active_clients.load(std::memory_order_acquire) > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const int left = active_clients.load(std::memory_order_acquire);
    if (left > 0) {
        std::cerr << "warning: " << context << ": " << left
                  << " active client(s) still running after " << timeout.count() << "s\n";
    }
}

}  // namespace

SqlServer::SqlServer(SqlServerConfig config) : config_(std::move(config)) {}

SqlServer::~SqlServer() {
    Stop();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    WaitForIdleClients(active_clients_, std::chrono::seconds(5), "SqlServer destructor");
}

lsmkv::Status SqlServer::Start() {
    if (listen_fd_ >= 0) {
        return STATUS(InvalidArgument, "server already started");
    }

    lsmkv::Options options;
    options.create_if_missing = true;

    // Keep db local until bind/listen succeed so failures do not leave partial state.
    std::shared_ptr<Database> db;
    RELDB_RETURN_NOT_OK(Database::Open(options, config_.db_path, &db));

    ScopedFd sock(::socket(AF_INET, SOCK_STREAM, 0));
    if (!sock) {
        return STATUS(IOError, std::string("socket() failed: ") + std::strerror(errno));
    }

    int yes = 1;
    ::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));
    if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) {
        return STATUS(InvalidArgument, "invalid host address: " + config_.host);
    }

    if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return STATUS(IOError, std::string("bind() failed: ") + std::strerror(errno));
    }

    if (::listen(sock.get(), 128) < 0) {
        return STATUS(IOError, std::string("listen() failed: ") + std::strerror(errno));
    }

    // Success: commit ownership to the SqlServer instance.
    listen_fd_ = sock.release();
    db_ = std::move(db);

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    } else {
        bound_port_ = config_.port;
    }

    running_.store(true, std::memory_order_release);
    return STATUS(OK);
}

bool SqlServer::TryReserveClientSlot() {
    int cur = active_clients_.load(std::memory_order_acquire);
    while (cur < config_.max_clients) {
        if (active_clients_.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void SqlServer::ReleaseClientSlot() {
    active_clients_.fetch_sub(1, std::memory_order_acq_rel);
}

void SqlServer::ServeClient(int client_fd) {
    struct SlotGuard {
        SqlServer* self;
        ~SlotGuard() { self->ReleaseClientSlot(); }
    } guard{this};

    SqlSession session(db_);
    std::string conn_buffer;
    std::string line;

    while (running_.load(std::memory_order_acquire)) {
        if (!RecvLine(client_fd, &line)) break;
        std::string reply;
        const bool keep_open = SqlHandleLine(session, conn_buffer, line, &reply);
        // Incomplete multi-line statements produce an empty reply — do not send.
        if (!reply.empty()) {
            if (!SendAll(client_fd, reply.data(), reply.size())) break;
        }
        if (!keep_open) break;
    }

    // Connection drop / QUIT: abort any open txn.
    (void)SqlResetSession(session, conn_buffer);
    ::close(client_fd);
}

lsmkv::Status SqlServer::Serve() {
    if (listen_fd_ < 0) {
        return STATUS(InvalidArgument, "Serve called before successful Start()");
    }

    while (running_.load(std::memory_order_acquire)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd_, &rfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;  // 200ms so Stop() is responsive
        const int ready = ::select(listen_fd_ + 1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::cerr << "select() failed: " << std::strerror(errno) << "\n";
            break;
        }
        if (ready == 0) continue;

        const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (!running_.load(std::memory_order_acquire)) break;
            std::cerr << "accept() failed: " << std::strerror(errno) << "\n";
            continue;
        }

        if (!TryReserveClientSlot()) {
            (void)SendLine(client_fd, "-ERR InvalidArgument: too many connections");
            ::close(client_fd);
            continue;
        }

        try {
            std::thread(&SqlServer::ServeClient, this, client_fd).detach();
        } catch (const std::system_error& e) {
            std::cerr << "failed to start client thread: " << e.what() << "\n";
            ::close(client_fd);
            ReleaseClientSlot();
        }
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    WaitForIdleClients(active_clients_, std::chrono::seconds(10), "SqlServer::Serve");
    return STATUS(OK);
}

void SqlServer::Stop() {
    running_.store(false, std::memory_order_release);
}

void PrintSqlServerUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " [--host HOST] [--port PORT] [--db PATH] [--max-clients N]\n"
              << "  --host         bind address (default 127.0.0.1)\n"
              << "  --port         TCP port (default 7380; 0 = ephemeral)\n"
              << "  --db           database directory (default ./reldb_sql_data)\n"
              << "  --max-clients  max concurrent connections (default 128)\n"
              << "\n"
              << "Wire protocol: line-oriented SQL until ';', plus PING/QUIT/RESET/STATUS.\n"
              << "No authentication — bind to localhost for local use.\n";
}

bool ParseSqlServerArgs(int argc, char** argv, SqlServerConfig* cfg, std::string* error) {
    if (cfg == nullptr || error == nullptr) return false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                *error = std::string("missing value for ") + name;
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--host") {
            const char* v = need("--host");
            if (!v) return false;
            cfg->host = v;
        } else if (arg == "--port") {
            const char* v = need("--port");
            if (!v) return false;
            cfg->port = std::atoi(v);
            if (cfg->port < 0 || cfg->port > 65535) {
                *error = "invalid port";
                return false;
            }
        } else if (arg == "--db") {
            const char* v = need("--db");
            if (!v) return false;
            cfg->db_path = v;
        } else if (arg == "--max-clients") {
            const char* v = need("--max-clients");
            if (!v) return false;
            cfg->max_clients = std::atoi(v);
            if (cfg->max_clients <= 0) {
                *error = "invalid --max-clients (must be >= 1)";
                return false;
            }
        } else if (arg == "-h" || arg == "--help") {
            PrintSqlServerUsage(argv[0]);
            std::exit(0);
        } else {
            *error = "unknown argument: " + arg;
            return false;
        }
    }
    return true;
}

}  // namespace reldb
