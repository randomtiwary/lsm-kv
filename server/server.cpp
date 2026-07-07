#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <system_error>
#include <thread>

namespace lsmkv {
namespace {

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
        if (out->size() > 1024 * 1024) {  // 1 MiB line cap
            return false;
        }
    }
    return true;
}

void SplitCommand(const std::string& line, std::string* cmd, std::string* rest) {
    cmd->clear();
    rest->clear();
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    *cmd = line.substr(start, i - start);
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    *rest = line.substr(i);
}

void SplitKeyValue(const std::string& s, std::string* key, std::string* value) {
    key->clear();
    value->clear();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    const std::size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    *key = s.substr(start, i - start);
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    *value = s.substr(i);
}

std::string FirstToken(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    const std::size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    return s.substr(start, i - start);
}

void AppendLine(std::string* out, const std::string& line) {
    out->append(line);
    out->push_back('\n');
}

}  // namespace

bool ExecuteRequest(DB* db, const std::string& line, std::string* reply) {
    reply->clear();
    std::string cmd;
    std::string rest;
    SplitCommand(line, &cmd, &rest);

    for (char& c : cmd) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }

    if (cmd.empty()) {
        AppendLine(reply, "-ERR empty request");
        return true;
    }

    if (cmd == "PING") {
        AppendLine(reply, "+PONG");
        return true;
    }

    if (cmd == "QUIT") {
        AppendLine(reply, "+OK");
        return false;
    }

    if (cmd == "GET") {
        const std::string key = FirstToken(rest);
        if (key.empty()) {
            AppendLine(reply, "-ERR usage: GET <key>");
            return true;
        }
        std::string value;
        const Status s = db->Get(ReadOptions(), key, &value);
        if (s.IsNotFound()) {
            AppendLine(reply, "NOT_FOUND");
            return true;
        }
        if (!s.ok()) {
            AppendLine(reply, "-ERR " + s.ToString());
            return true;
        }
        AppendLine(reply, "$" + std::to_string(value.size()));
        AppendLine(reply, value);
        return true;
    }

    if (cmd == "SET") {
        std::string key;
        std::string value;
        SplitKeyValue(rest, &key, &value);
        if (key.empty()) {
            AppendLine(reply, "-ERR usage: SET <key> <value>");
            return true;
        }
        const Status s = db->Put(WriteOptions(), key, value);
        if (!s.ok()) {
            AppendLine(reply, "-ERR " + s.ToString());
            return true;
        }
        AppendLine(reply, "+OK");
        return true;
    }

    if (cmd == "DEL") {
        const std::string key = FirstToken(rest);
        if (key.empty()) {
            AppendLine(reply, "-ERR usage: DEL <key>");
            return true;
        }
        const Status s = db->Delete(WriteOptions(), key);
        if (!s.ok()) {
            AppendLine(reply, "-ERR " + s.ToString());
            return true;
        }
        AppendLine(reply, "+OK");
        return true;
    }

    AppendLine(reply, "-ERR unknown command '" + cmd + "'");
    return true;
}

Server::Server(ServerConfig config) : config_(std::move(config)) {}

Server::~Server() {
    Stop();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    // Wait briefly for handlers if Serve() was stopped without joining them.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (active_clients_.load(std::memory_order_acquire) > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

Status Server::Start() {
    if (listen_fd_ >= 0) {
        return Status::InvalidArgument("server already started");
    }

    Options options;
    options.create_if_missing = true;

    DB* raw = nullptr;
    Status s = DB::Open(options, config_.db_path, &raw);
    if (!s.ok()) return s;
    db_.reset(raw);

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        db_.reset();
        return Status::IOError(std::string("socket() failed: ") + std::strerror(errno));
    }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));
    if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        db_.reset();
        return Status::InvalidArgument("invalid host address: " + config_.host);
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const std::string msg = std::string("bind() failed: ") + std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        db_.reset();
        return Status::IOError(msg);
    }

    if (::listen(listen_fd_, 128) < 0) {
        const std::string msg = std::string("listen() failed: ") + std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        db_.reset();
        return Status::IOError(msg);
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    } else {
        bound_port_ = config_.port;
    }

    running_.store(true, std::memory_order_release);
    return Status::OK();
}

bool Server::TryReserveClientSlot() {
    int cur = active_clients_.load(std::memory_order_acquire);
    while (cur < config_.max_clients) {
        if (active_clients_.compare_exchange_weak(
                cur, cur + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void Server::ReleaseClientSlot() {
    active_clients_.fetch_sub(1, std::memory_order_acq_rel);
}

void Server::ServeClient(int client_fd) {
    // Adopts a pre-reserved active_clients_ slot; release on exit.
    struct SlotGuard {
        Server* self;
        ~SlotGuard() { self->ReleaseClientSlot(); }
    } guard{this};

    std::string line;
    while (running_.load(std::memory_order_acquire)) {
        if (!RecvLine(client_fd, &line)) break;
        std::string reply;
        const bool keep_open = ExecuteRequest(db_.get(), line, &reply);
        if (!SendAll(client_fd, reply.data(), reply.size())) break;
        if (!keep_open) break;
    }
    ::close(client_fd);
}

void Server::Serve() {
    if (listen_fd_ < 0) return;

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
            SendLine(client_fd, "-ERR too many connections");
            ::close(client_fd);
            continue;
        }

        try {
            std::thread(&Server::ServeClient, this, client_fd).detach();
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

    // Wait for in-flight handlers before allowing db_ destruction in ~Server / after Serve.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (active_clients_.load(std::memory_order_acquire) > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Server::Stop() {
    running_.store(false, std::memory_order_release);
    // Wake a blocking accept/select by connecting to ourselves is unnecessary
    // with the short select timeout; closing listen_fd is done in Serve() exit.
}

void PrintServerUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " [--host HOST] [--port PORT] [--db PATH] [--max-clients N]\n"
              << "  --host         bind address (default 0.0.0.0)\n"
              << "  --port         TCP port (default 7379; 0 = ephemeral)\n"
              << "  --db           database directory (default ./lsmkv_data)\n"
              << "  --max-clients  max concurrent connections (default 128)\n";
}

bool ParseServerArgs(int argc, char** argv, ServerConfig* cfg, std::string* error) {
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
            PrintServerUsage(argv[0]);
            std::exit(0);
        } else {
            *error = "unknown argument: " + arg;
            return false;
        }
    }
    return true;
}

}  // namespace lsmkv
