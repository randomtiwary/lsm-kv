// Minimal TCP server for the lsm-kv embedded engine.
//
// Wire protocol (one request per line, '\n'-terminated; no '\n' in keys/values):
//   PING
//   GET <key>
//   SET <key> <value>   // value is the remainder of the line after "<key> "
//   DEL <key>
//   QUIT
//
// Responses (also line-oriented):
//   +OK
//   +PONG
//   $N                 // successful GET; next line is exactly N bytes of value
//   $-1                // key not found
//   -ERR <message>
//
// Usage:
//   lsmkv_server [--port PORT] [--host HOST] [--db PATH]
// Defaults: host=0.0.0.0, port=7379, db=./lsmkv_data

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "lsmkv/db.h"

namespace {

std::atomic<bool> g_running{true};

void OnSignal(int /*sig*/) { g_running.store(false); }

// --- tiny helpers -----------------------------------------------------------

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
        if (r == 0) return false;  // peer closed
        if (ch == '\n') break;
        if (ch != '\r') out->push_back(ch);
        if (out->size() > 1024 * 1024) {  // 1 MiB line cap
            return false;
        }
    }
    return true;
}

// First whitespace-delimited token; rest is everything after the following space(s) skipped once.
// "SET k v has spaces" -> cmd="SET", arg="k v has spaces"
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

// First token as key; remainder (after whitespace) as value. Empty value allowed.
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

// --- request handling -------------------------------------------------------

bool HandleRequest(lsmkv::DB* db, int fd, const std::string& line) {
    std::string cmd;
    std::string rest;
    SplitCommand(line, &cmd, &rest);

    // Upper-case command for case-insensitive matching.
    for (char& c : cmd) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }

    if (cmd.empty()) {
        return SendLine(fd, "-ERR empty request");
    }

    if (cmd == "PING") {
        return SendLine(fd, "+PONG");
    }

    if (cmd == "QUIT") {
        SendLine(fd, "+OK");
        return false;  // close connection
    }

    if (cmd == "GET") {
        const std::string key = FirstToken(rest);
        if (key.empty()) return SendLine(fd, "-ERR usage: GET <key>");
        std::string value;
        const lsmkv::Status s = db->Get(lsmkv::ReadOptions(), key, &value);
        if (s.IsNotFound()) return SendLine(fd, "$-1");
        if (!s.ok()) return SendLine(fd, "-ERR " + s.ToString());
        if (!SendLine(fd, "$" + std::to_string(value.size()))) return false;
        // Value line: raw bytes + newline terminator (value itself must not contain '\n').
        return SendLine(fd, value);
    }

    if (cmd == "SET") {
        std::string key;
        std::string value;
        SplitKeyValue(rest, &key, &value);
        if (key.empty()) return SendLine(fd, "-ERR usage: SET <key> <value>");
        const lsmkv::Status s = db->Put(lsmkv::WriteOptions(), key, value);
        if (!s.ok()) return SendLine(fd, "-ERR " + s.ToString());
        return SendLine(fd, "+OK");
    }

    if (cmd == "DEL") {
        const std::string key = FirstToken(rest);
        if (key.empty()) return SendLine(fd, "-ERR usage: DEL <key>");
        const lsmkv::Status s = db->Delete(lsmkv::WriteOptions(), key);
        if (!s.ok()) return SendLine(fd, "-ERR " + s.ToString());
        return SendLine(fd, "+OK");
    }

    return SendLine(fd, "-ERR unknown command '" + cmd + "'");
}

void ServeClient(lsmkv::DB* db, int client_fd) {
    // Disable SIGPIPE per-send via MSG_NOSIGNAL where available; also ignore globally.
    std::string line;
    while (g_running.load()) {
        if (!RecvLine(client_fd, &line)) break;
        if (!HandleRequest(db, client_fd, line)) break;
    }
    ::close(client_fd);
}

struct Config {
    std::string host = "0.0.0.0";
    int port = 7379;
    std::string db_path = "./lsmkv_data";
};

void PrintUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " [--host HOST] [--port PORT] [--db PATH]\n"
              << "  --host  bind address (default 0.0.0.0)\n"
              << "  --port  TCP port (default 7379)\n"
              << "  --db    database directory (default ./lsmkv_data)\n";
}

bool ParseArgs(int argc, char** argv, Config* cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
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
            if (cfg->port <= 0 || cfg->port > 65535) {
                std::cerr << "invalid port\n";
                return false;
            }
        } else if (arg == "--db") {
            const char* v = need("--db");
            if (!v) return false;
            cfg->db_path = v;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            PrintUsage(argv[0]);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!ParseArgs(argc, argv, &cfg)) return 2;

    // Avoid process death on client disconnect mid-write.
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, OnSignal);
    ::signal(SIGTERM, OnSignal);

    lsmkv::Options options;
    options.create_if_missing = true;

    lsmkv::DB* db = nullptr;
    lsmkv::Status s = lsmkv::DB::Open(options, cfg.db_path, &db);
    if (!s.ok()) {
        std::cerr << "DB::Open(" << cfg.db_path << ") failed: " << s.ToString() << "\n";
        return 1;
    }

    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
        delete db;
        return 1;
    }

    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg.port));
    if (::inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "invalid --host address: " << cfg.host << "\n";
        ::close(listen_fd);
        delete db;
        return 1;
    }

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed: " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        delete db;
        return 1;
    }

    if (::listen(listen_fd, 128) < 0) {
        std::cerr << "listen() failed: " << std::strerror(errno) << "\n";
        ::close(listen_fd);
        delete db;
        return 1;
    }

    std::cout << "lsmkv_server listening on " << cfg.host << ":" << cfg.port
              << "  db=" << cfg.db_path << std::endl;

    // Accept loop: one thread per client. DB handles concurrent Get/Put safely.
    while (g_running.load()) {
        // Use a short accept timeout so we notice shutdown.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        const int ready = ::select(listen_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::cerr << "select() failed: " << std::strerror(errno) << "\n";
            break;
        }
        if (ready == 0) continue;

        const int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (!g_running.load()) break;
            std::cerr << "accept() failed: " << std::strerror(errno) << "\n";
            continue;
        }

        // Detach so we do not accumulate joinable threads for long-lived servers.
        std::thread(ServeClient, db, client_fd).detach();
    }

    ::close(listen_fd);
    delete db;
    std::cout << "lsmkv_server shut down" << std::endl;
    return 0;
}
