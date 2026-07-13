#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include "sql_server.h"
#include "test_harness.h"
#include "test_util.h"

namespace {

bool SendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t r = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (r <= 0) return false;
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

bool RecvLine(int fd, std::string* line, std::chrono::milliseconds timeout) {
    line->clear();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        if (::select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
        char ch;
        const ssize_t r = ::recv(fd, &ch, 1, 0);
        if (r <= 0) return false;
        if (ch == '\n') {
            if (!line->empty() && line->back() == '\r') line->pop_back();
            return true;
        }
        line->push_back(ch);
        if (line->size() > 1024 * 1024) return false;
    }
}

// Recv until a line equals end_marker (e.g. "*END") or timeout; append all lines + \n.
bool RecvUntilLine(int fd, const std::string& end_marker, std::string* out,
                   std::chrono::milliseconds timeout) {
    out->clear();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string line;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        if (!RecvLine(fd, &line, left)) return false;
        out->append(line);
        out->push_back('\n');
        if (line == end_marker) return true;
    }
    return false;
}

int ConnectLocal(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

struct RunningSqlServer {
    std::string db_path;
    reldb::SqlServer server;
    std::thread thr;
    int port = 0;

    RunningSqlServer()
        : db_path(MakeTempDir("reldb_sql_srv")), server(MakeConfig(db_path)) {
        const auto s = server.Start();
        if (!s.ok()) {
            std::cerr << "SqlServer::Start failed: " << s.ToString() << "\n";
            return;
        }
        port = server.port();
        thr = std::thread([this] { server.Serve(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    static reldb::SqlServerConfig MakeConfig(const std::string& path) {
        reldb::SqlServerConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 0;
        cfg.db_path = path;
        cfg.max_clients = 8;
        return cfg;
    }

    ~RunningSqlServer() {
        server.Stop();
        if (thr.joinable()) thr.join();
        RemoveDirRecursive(db_path);
    }
};

}  // namespace

TEST(sql_server_parse_args) {
    reldb::SqlServerConfig cfg;
    std::string err;
    char* argv[] = {
        const_cast<char*>("reldb_sql_server"),
        const_cast<char*>("--host"),
        const_cast<char*>("127.0.0.1"),
        const_cast<char*>("--port"),
        const_cast<char*>("7381"),
        const_cast<char*>("--db"),
        const_cast<char*>("/tmp/sqlx"),
        const_cast<char*>("--max-clients"),
        const_cast<char*>("4"),
    };
    expect(reldb::ParseSqlServerArgs(9, argv, &cfg, &err), "parse ok");
    expect_eq(cfg.host, std::string("127.0.0.1"), "host");
    expect_eq(cfg.port, 7381, "port");
    expect_eq(cfg.db_path, std::string("/tmp/sqlx"), "db");
    expect_eq(cfg.max_clients, 4, "max clients");

    // Defaults
    reldb::SqlServerConfig def;
    expect_eq(def.port, 7380, "default port");
    expect_eq(def.host, std::string("127.0.0.1"), "default host");
}

TEST(sql_server_tcp_ping_sql_quit) {
    RunningSqlServer rs;
    expect(rs.port > 0, "bound port");

    const int fd = ConnectLocal(rs.port);
    expect(fd >= 0, "connect");
    if (fd < 0) return;

    std::string line;
    expect(SendAll(fd, "PING\n"), "send ping");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv pong");
    expect_eq(line, std::string("+PONG"), "pong");

    expect(SendAll(fd, "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);\n"), "create");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv create");
    expect_eq(line, std::string("+OK"), "create ok");

    expect(SendAll(fd, "INSERT INTO users VALUES (1, 'ada');\n"), "insert");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv insert");
    expect_eq(line, std::string("+OK rows_affected=1"), "insert ok");

    // Multi-line SELECT
    expect(SendAll(fd, "SELECT id, name\n"), "sel1");
    // No complete statement yet — server should not reply.
    // Complete it:
    expect(SendAll(fd, "FROM users WHERE id = 1;\n"), "sel2");
    std::string result;
    expect(RecvUntilLine(fd, "*END", &result, std::chrono::seconds(3)), "recv result");
    expect(result.find("*RESULT 1 2\n") != std::string::npos, "result hdr");
    expect(result.find("$I:1\n") != std::string::npos, "id");
    expect(result.find("$S:ada\n") != std::string::npos, "name");

    expect(SendAll(fd, "STATUS\n"), "status");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv status");
    expect_eq(line, std::string("+OK in_txn=0"), "status");

    expect(SendAll(fd, "QUIT\n"), "quit");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv quit");
    expect_eq(line, std::string("+OK"), "quit ok");

    ::close(fd);
}

TEST(sql_server_tcp_txn_stickiness) {
    RunningSqlServer rs;
    expect(rs.port > 0, "bound");

    const int fd = ConnectLocal(rs.port);
    expect(fd >= 0, "connect");
    if (fd < 0) return;

    std::string line;
    expect(SendAll(fd, "CREATE TABLE t(id INT PRIMARY KEY, n INT);\n"), "create");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "c");
    expect_eq(line, std::string("+OK"), "cok");

    expect(SendAll(fd, "BEGIN;\n"), "begin");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "b");
    expect_eq(line, std::string("+OK"), "bok");

    expect(SendAll(fd, "STATUS\n"), "st");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "st r");
    expect_eq(line, std::string("+OK in_txn=1"), "in txn");

    expect(SendAll(fd, "INSERT INTO t VALUES (1, 10);\n"), "ins");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "ins r");

    expect(SendAll(fd, "COMMIT;\n"), "commit");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "commit r");
    expect_eq(line, std::string("+OK"), "commit ok");

    expect(SendAll(fd, "QUIT\n"), "quit");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "q");
    ::close(fd);
}
