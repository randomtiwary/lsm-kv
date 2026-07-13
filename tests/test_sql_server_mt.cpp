// A4: multi-client SQL server tests over localhost TCP.
// Covers write-write conflict across sessions, concurrent SELECT + CREATE,
// and concurrent disjoint inserts (catalog + SI safety under multi-client).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// Read one full protocol reply: single-line (+OK / -ERR / +PONG) or result block to *END.
bool RecvReply(int fd, std::string* out, std::chrono::milliseconds timeout) {
    out->clear();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string line;
    auto left = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                     std::chrono::steady_clock::now());
    };
    if (left().count() <= 0) return false;
    if (!RecvLine(fd, &line, left())) return false;
    out->append(line);
    out->push_back('\n');

    if (line.rfind("*PLAN", 0) == 0 || line.rfind("*RESULT", 0) == 0) {
        while (line != "*END") {
            if (left().count() <= 0) return false;
            if (!RecvLine(fd, &line, left())) return false;
            out->append(line);
            out->push_back('\n');
        }
    }
    return true;
}

// Send one request line (with trailing \n if missing) and read the full reply.
bool RoundTrip(int fd, std::string req, std::string* reply,
               std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    if (req.empty() || req.back() != '\n') req.push_back('\n');
    if (!SendAll(fd, req)) return false;
    return RecvReply(fd, reply, timeout);
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

    explicit RunningSqlServer(int max_clients = 32)
        : db_path(MakeTempDir("reldb_sql_mt")), server(MakeConfig(db_path, max_clients)) {
        const auto s = server.Start();
        if (!s.ok()) {
            std::cerr << "SqlServer::Start failed: " << s.ToString() << "\n";
            return;
        }
        port = server.port();
        thr = std::thread([this] {
            const auto st = server.Serve();
            if (!st.ok()) {
                std::cerr << "SqlServer::Serve failed: " << st.ToString() << "\n";
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    static reldb::SqlServerConfig MakeConfig(const std::string& path, int max_clients) {
        reldb::SqlServerConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 0;
        cfg.db_path = path;
        cfg.max_clients = max_clients;
        return cfg;
    }

    ~RunningSqlServer() {
        server.Stop();
        if (thr.joinable()) thr.join();
        RemoveDirRecursive(db_path);
    }
};

bool StartsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

}  // namespace

// Two clients open transactions and both UPDATE the same PK.
// First writer holds the provisional; second must get Conflict (early WW).
TEST(sql_server_mt_write_write_conflict) {
    RunningSqlServer rs;
    expect(rs.port > 0, "bound");

    const int setup = ConnectLocal(rs.port);
    expect(setup >= 0, "setup connect");
    if (setup < 0) return;

    std::string reply;
    expect(RoundTrip(setup, "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);", &reply), "create");
    expect(StartsWith(reply, "+OK"), "create ok");
    expect(RoundTrip(setup, "INSERT INTO users VALUES (1, 'seed');", &reply), "seed");
    expect(StartsWith(reply, "+OK rows_affected=1"), "seed ok");
    expect(RoundTrip(setup, "QUIT", &reply), "setup quit");
    ::close(setup);

    const int a = ConnectLocal(rs.port);
    const int b = ConnectLocal(rs.port);
    expect(a >= 0 && b >= 0, "clients");
    if (a < 0 || b < 0) return;

    expect(RoundTrip(a, "BEGIN;", &reply), "a begin");
    expect(StartsWith(reply, "+OK"), "a begin ok");
    expect(RoundTrip(b, "BEGIN;", &reply), "b begin");
    expect(StartsWith(reply, "+OK"), "b begin ok");

    expect(RoundTrip(a, "UPDATE users SET name = 'from_a' WHERE id = 1;", &reply), "a update");
    expect(StartsWith(reply, "+OK rows_affected=1"), "a update ok");

    // B conflicts while A still holds the open write.
    expect(RoundTrip(b, "UPDATE users SET name = 'from_b' WHERE id = 1;", &reply), "b update");
    expect(StartsWith(reply, "-ERR Conflict:"), "b conflict");
    expect(reply.find("write-write conflict") != std::string::npos, "ww message");

    expect(RoundTrip(a, "COMMIT;", &reply), "a commit");
    expect(StartsWith(reply, "+OK"), "a commit ok");

    // B should abort its txn and see A's value on a new autocommit read.
    expect(RoundTrip(b, "ABORT;", &reply), "b abort");
    // After conflict the session may still be in a failed txn state — ABORT cleans up.
    expect(RoundTrip(b, "SELECT name FROM users WHERE id = 1;", &reply), "b select");
    expect(reply.find("$S:from_a\n") != std::string::npos, "sees a value");

    expect(RoundTrip(a, "QUIT", &reply), "a quit");
    expect(RoundTrip(b, "QUIT", &reply), "b quit");
    ::close(a);
    ::close(b);
}

// Concurrent CREATE of distinct tables while another client SELECTs a seed table.
TEST(sql_server_mt_concurrent_create_and_select) {
    RunningSqlServer rs(32);
    expect(rs.port > 0, "bound");

    const int setup = ConnectLocal(rs.port);
    expect(setup >= 0, "setup");
    if (setup < 0) return;
    std::string reply;
    expect(RoundTrip(setup, "CREATE TABLE seed(id INT PRIMARY KEY, v INT);", &reply), "seed t");
    expect(RoundTrip(setup, "INSERT INTO seed VALUES (1, 42);", &reply), "seed row");
    expect(RoundTrip(setup, "QUIT", &reply), "setup quit");
    ::close(setup);

    constexpr int kCreators = 8;
    std::atomic<int> create_ok{0};
    std::atomic<int> select_ok{0};
    std::atomic<int> errors{0};
    std::atomic<bool> stop{false};

    std::thread reader([&]() {
        const int fd = ConnectLocal(rs.port);
        if (fd < 0) {
            errors.fetch_add(1);
            return;
        }
        std::string r;
        while (!stop.load(std::memory_order_acquire)) {
            if (!RoundTrip(fd, "SELECT v FROM seed WHERE id = 1;", &r, std::chrono::seconds(3))) {
                errors.fetch_add(1);
                break;
            }
            if (r.find("$I:42\n") != std::string::npos) {
                select_ok.fetch_add(1);
            } else if (!StartsWith(r, "-ERR")) {
                // empty or unexpected — count as soft error only if clearly wrong
                if (r.find("*RESULT") != std::string::npos && r.find("$I:42") == std::string::npos) {
                    errors.fetch_add(1);
                }
            } else {
                errors.fetch_add(1);
            }
        }
        (void)RoundTrip(fd, "QUIT", &r, std::chrono::seconds(2));
        ::close(fd);
    });

    std::vector<std::thread> creators;
    for (int i = 0; i < kCreators; ++i) {
        creators.emplace_back([&, i]() {
            const int fd = ConnectLocal(rs.port);
            if (fd < 0) {
                errors.fetch_add(1);
                return;
            }
            std::string r;
            const std::string sql =
                "CREATE TABLE t" + std::to_string(i) + "(id INT PRIMARY KEY, name TEXT);";
            // Autocommit SELECT briefly elevates open_txn_count_ and can reject
            // DDL. Retry until CREATE succeeds or we give up.
            bool ok = false;
            for (int attempt = 0; attempt < 200; ++attempt) {
                if (!RoundTrip(fd, sql, &r, std::chrono::seconds(5))) {
                    errors.fetch_add(1);
                    break;
                }
                if (StartsWith(r, "+OK")) {
                    ok = true;
                    create_ok.fetch_add(1);
                    break;
                }
                if (r.find("DDL requires no open transactions") != std::string::npos) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                errors.fetch_add(1);
                break;
            }
            if (!ok && errors.load() == 0) {
                // Timed out retrying only open-txn gate.
                errors.fetch_add(1);
            }
            (void)RoundTrip(fd, "QUIT", &r, std::chrono::seconds(2));
            ::close(fd);
        });
    }
    for (auto& th : creators) th.join();
    stop.store(true, std::memory_order_release);
    reader.join();

    expect_eq(create_ok.load(), kCreators, "all creates");
    expect(select_ok.load() > 0, "some selects ok");
    expect_eq(errors.load(), 0, "no errors");

    // Verify tables exist via a fresh client.
    const int verify = ConnectLocal(rs.port);
    expect(verify >= 0, "verify connect");
    if (verify >= 0) {
        for (int i = 0; i < kCreators; ++i) {
            // INSERT proves table exists (empty table select also works).
            const std::string sql =
                "INSERT INTO t" + std::to_string(i) + " VALUES (1, 'x');";
            expect(RoundTrip(verify, sql, &reply), "ins");
            expect(StartsWith(reply, "+OK"), "table exists");
        }
        (void)RoundTrip(verify, "QUIT", &reply);
        ::close(verify);
    }
}

// Many clients insert disjoint primary keys concurrently (autocommit).
TEST(sql_server_mt_concurrent_disjoint_inserts) {
    RunningSqlServer rs(32);
    expect(rs.port > 0, "bound");

    const int setup = ConnectLocal(rs.port);
    expect(setup >= 0, "setup");
    if (setup < 0) return;
    std::string reply;
    expect(RoundTrip(setup, "CREATE TABLE items(id INT PRIMARY KEY, name TEXT);", &reply), "create");
    expect(RoundTrip(setup, "QUIT", &reply), "quit setup");
    ::close(setup);

    constexpr int kClients = 6;
    constexpr int kPerClient = 20;
    std::atomic<int> ok{0};
    std::atomic<int> fail{0};

    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&, c]() {
            const int fd = ConnectLocal(rs.port);
            if (fd < 0) {
                fail.fetch_add(1);
                return;
            }
            std::string r;
            for (int i = 0; i < kPerClient; ++i) {
                const int id = c * 1000 + i;
                const std::string sql =
                    "INSERT INTO items VALUES (" + std::to_string(id) + ", 'n');";
                if (!RoundTrip(fd, sql, &r, std::chrono::seconds(5))) {
                    fail.fetch_add(1);
                    continue;
                }
                if (StartsWith(r, "+OK rows_affected=1")) {
                    ok.fetch_add(1);
                } else {
                    fail.fetch_add(1);
                }
            }
            (void)RoundTrip(fd, "QUIT", &r, std::chrono::seconds(2));
            ::close(fd);
        });
    }
    for (auto& th : threads) th.join();

    expect_eq(fail.load(), 0, "no failures");
    expect_eq(ok.load(), kClients * kPerClient, "all inserts");

    // Count rows via scan.
    const int v = ConnectLocal(rs.port);
    expect(v >= 0, "verify");
    if (v >= 0) {
        expect(RoundTrip(v, "SELECT id FROM items;", &reply, std::chrono::seconds(5)), "scan");
        // *RESULT N 1
        expect(reply.find("*RESULT " + std::to_string(kClients * kPerClient) + " 1\n") !=
                   std::string::npos,
               "row count");
        (void)RoundTrip(v, "QUIT", &reply);
        ::close(v);
    }
}
