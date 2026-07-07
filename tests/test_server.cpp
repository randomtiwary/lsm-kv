#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "server.h"
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

// Read one line (without trailing \n). Returns false on timeout/EOF.
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

struct RunningServer {
    std::string db_path;
    lsmkv::Server server;
    std::thread thr;
    int port = 0;

    explicit RunningServer(int max_clients = 8)
        : db_path(MakeTempDir("lsmkv_srv")),
          server(MakeConfig(db_path, max_clients)) {
        const lsmkv::Status s = server.Start();
        if (!s.ok()) {
            std::cerr << "Server::Start failed: " << s.ToString() << "\n";
            return;
        }
        port = server.port();
        thr = std::thread([this] { server.Serve(); });
        // Brief pause for accept loop to enter select.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    static lsmkv::ServerConfig MakeConfig(const std::string& path, int max_clients) {
        lsmkv::ServerConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 0;  // ephemeral
        cfg.db_path = path;
        cfg.max_clients = max_clients;
        return cfg;
    }

    ~RunningServer() {
        server.Stop();
        if (thr.joinable()) thr.join();
        RemoveDirRecursive(db_path);
    }
};

std::string FirstLine(const std::string& reply) {
    const auto pos = reply.find('\n');
    if (pos == std::string::npos) return reply;
    std::string line = reply.substr(0, pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

}  // namespace

// --- protocol unit tests (no network) ---------------------------------------

TEST(server_protocol_ping_and_unknown) {
    const std::string dir = MakeTempDir("lsmkv_proto");
    lsmkv::Options opt;
    opt.create_if_missing = true;
    lsmkv::DB* raw = nullptr;
    expect(lsmkv::DB::Open(opt, dir, &raw).ok(), "open db");
    std::unique_ptr<lsmkv::DB> db(raw);

    std::string reply;
    expect(lsmkv::ExecuteRequest(db.get(), "PING", &reply), "ping keep open");
    expect_eq(FirstLine(reply), std::string("+PONG"), "ping reply");

    expect(lsmkv::ExecuteRequest(db.get(), "ping", &reply), "ping case");
    expect_eq(FirstLine(reply), std::string("+PONG"), "ping lower case");

    expect(lsmkv::ExecuteRequest(db.get(), "FOO", &reply), "unknown keep open");
    expect(FirstLine(reply).find("-ERR") == 0, "unknown err prefix");

    expect(!lsmkv::ExecuteRequest(db.get(), "QUIT", &reply), "quit closes");
    expect_eq(FirstLine(reply), std::string("+OK"), "quit ok");

    RemoveDirRecursive(dir);
}

TEST(server_protocol_set_get_del) {
    const std::string dir = MakeTempDir("lsmkv_proto2");
    lsmkv::Options opt;
    opt.create_if_missing = true;
    lsmkv::DB* raw = nullptr;
    expect(lsmkv::DB::Open(opt, dir, &raw).ok(), "open db");
    std::unique_ptr<lsmkv::DB> db(raw);

    std::string reply;
    expect(lsmkv::ExecuteRequest(db.get(), "GET missing", &reply), "get miss keep");
    expect_eq(FirstLine(reply), std::string("NOT_FOUND"), "not found");

    expect(lsmkv::ExecuteRequest(db.get(), "SET hello world", &reply), "set");
    expect_eq(FirstLine(reply), std::string("+OK"), "set ok");

    expect(lsmkv::ExecuteRequest(db.get(), "GET hello", &reply), "get");
    // reply is "$5\nworld\n"
    expect(reply.find("$5\n") == 0 || reply.find("$5\r\n") == 0, "get length line");
    expect(reply.find("world") != std::string::npos, "get value");

    expect(lsmkv::ExecuteRequest(db.get(), "SET a b c d", &reply), "set spaces");
    expect(lsmkv::ExecuteRequest(db.get(), "GET a", &reply), "get spaces");
    expect(reply.find("b c d") != std::string::npos, "value has spaces");

    expect(lsmkv::ExecuteRequest(db.get(), "DEL hello", &reply), "del");
    expect_eq(FirstLine(reply), std::string("+OK"), "del ok");
    expect(lsmkv::ExecuteRequest(db.get(), "GET hello", &reply), "get after del");
    expect_eq(FirstLine(reply), std::string("NOT_FOUND"), "gone");

    expect(lsmkv::ExecuteRequest(db.get(), "SET", &reply), "set usage");
    expect(FirstLine(reply).find("-ERR") == 0, "set needs key");
    expect(lsmkv::ExecuteRequest(db.get(), "GET", &reply), "get usage");
    expect(FirstLine(reply).find("-ERR") == 0, "get needs key");

    RemoveDirRecursive(dir);
}

TEST(server_parse_args) {
    lsmkv::ServerConfig cfg;
    std::string err;
    char* argv[] = {
        const_cast<char*>("lsmkv_server"),
        const_cast<char*>("--host"),
        const_cast<char*>("127.0.0.1"),
        const_cast<char*>("--port"),
        const_cast<char*>("9001"),
        const_cast<char*>("--db"),
        const_cast<char*>("/tmp/x"),
        const_cast<char*>("--max-clients"),
        const_cast<char*>("4"),
    };
    expect(lsmkv::ParseServerArgs(9, argv, &cfg, &err), "parse ok");
    expect_eq(cfg.host, std::string("127.0.0.1"), "host");
    expect_eq(cfg.port, 9001, "port");
    expect_eq(cfg.db_path, std::string("/tmp/x"), "db");
    expect_eq(cfg.max_clients, 4, "max clients");

    char* bad[] = {const_cast<char*>("lsmkv_server"), const_cast<char*>("--nope")};
    expect(!lsmkv::ParseServerArgs(2, bad, &cfg, &err), "unknown arg");
}

// --- integration tests over TCP ---------------------------------------------

TEST(server_tcp_set_get_del_ping) {
    RunningServer rs;
    expect(rs.port > 0, "bound port");

    const int fd = ConnectLocal(rs.port);
    expect(fd >= 0, "connect");
    if (fd < 0) return;

    expect(SendAll(fd, "PING\n"), "send ping");
    std::string line;
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv pong");
    expect_eq(line, std::string("+PONG"), "pong");

    expect(SendAll(fd, "SET k v1\n"), "send set");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv set");
    expect_eq(line, std::string("+OK"), "set ok");

    expect(SendAll(fd, "GET k\n"), "send get");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv get len");
    expect_eq(line, std::string("$2"), "len");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv get val");
    expect_eq(line, std::string("v1"), "val");

    expect(SendAll(fd, "DEL k\n"), "send del");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv del");
    expect_eq(line, std::string("+OK"), "del ok");

    expect(SendAll(fd, "GET k\n"), "send get miss");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv miss");
    expect_eq(line, std::string("NOT_FOUND"), "miss");

    expect(SendAll(fd, "QUIT\n"), "send quit");
    expect(RecvLine(fd, &line, std::chrono::seconds(2)), "recv quit");
    expect_eq(line, std::string("+OK"), "quit");
    ::close(fd);
}

TEST(server_tcp_max_clients) {
    RunningServer rs(/*max_clients=*/2);
    expect(rs.port > 0, "bound port");

    const int fd1 = ConnectLocal(rs.port);
    const int fd2 = ConnectLocal(rs.port);
    expect(fd1 >= 0 && fd2 >= 0, "two connections");
    if (fd1 < 0 || fd2 < 0) {
        if (fd1 >= 0) ::close(fd1);
        if (fd2 >= 0) ::close(fd2);
        return;
    }

    // Hold slots open until the server has accepted both (active_clients >= 2).
    for (int i = 0; i < 50 && rs.server.active_clients() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    expect(rs.server.active_clients() >= 2, "two active clients");

    const int fd3 = ConnectLocal(rs.port);
    expect(fd3 >= 0, "third connect at tcp level");
    if (fd3 >= 0) {
        std::string line;
        expect(RecvLine(fd3, &line, std::chrono::seconds(2)), "recv reject");
        expect_eq(line, std::string("-ERR too many connections"), "rejected");
        ::close(fd3);
    }

    ::close(fd1);
    ::close(fd2);

    // After release, a new client should succeed.
    for (int i = 0; i < 50 && rs.server.active_clients() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const int fd4 = ConnectLocal(rs.port);
    expect(fd4 >= 0, "reconnect after free");
    if (fd4 >= 0) {
        expect(SendAll(fd4, "PING\n"), "ping after free");
        std::string line;
        expect(RecvLine(fd4, &line, std::chrono::seconds(2)), "pong after free");
        expect_eq(line, std::string("+PONG"), "pong ok");
        ::close(fd4);
    }
}

TEST(server_tcp_persistence_across_clients) {
    RunningServer rs;
    expect(rs.port > 0, "bound port");

    {
        const int fd = ConnectLocal(rs.port);
        expect(fd >= 0, "c1");
        if (fd >= 0) {
            expect(SendAll(fd, "SET shared value\n"), "set");
            std::string line;
            expect(RecvLine(fd, &line, std::chrono::seconds(2)), "set reply");
            expect_eq(line, std::string("+OK"), "ok");
            ::close(fd);
        }
    }
    {
        const int fd = ConnectLocal(rs.port);
        expect(fd >= 0, "c2");
        if (fd >= 0) {
            expect(SendAll(fd, "GET shared\n"), "get");
            std::string line;
            expect(RecvLine(fd, &line, std::chrono::seconds(2)), "len");
            expect_eq(line, std::string("$5"), "len 5");
            expect(RecvLine(fd, &line, std::chrono::seconds(2)), "val");
            expect_eq(line, std::string("value"), "same db");
            ::close(fd);
        }
    }
}
