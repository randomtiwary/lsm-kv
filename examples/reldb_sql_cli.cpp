// Network SQL client for reldb_sql_server (roadmap A5).
//
//   reldb_sql_cli [--host HOST] [--port PORT] [-c SQL] [--file PATH]
//
// Interactive (default): type SQL ended by ';'. Local meta: .help .quit
// Server meta (empty buffer): PING STATUS RESET QUIT
// Non-interactive: -c and/or --file (statements must end with ';')
// Does not replace reldb_sql_shell (local embedded DB).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "protocol.h"
#include "reldb/query_format.h"
#include "reldb/string_util.h"
#include "reldb/types.h"

namespace {

void PrintUsage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [--host HOST] [--port PORT] [-c SQL] [--file PATH]\n"
        << "  --host HOST   server host (default 127.0.0.1)\n"
        << "  --port PORT   server port (default 7380)\n"
        << "  -c SQL        execute SQL (may contain multiple statements) and exit\n"
        << "  --file PATH   execute SQL from file and exit\n"
        << "  (neither)     interactive mode on stdin\n"
        << "\n"
        << "Connects to reldb_sql_server. End SQL with ';'.\n"
        << "Local: .help .quit   Server: PING STATUS RESET QUIT\n";
}

bool SendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t r = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (r <= 0) return false;
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

bool RecvLine(int fd, std::string* line) {
    line->clear();
    char ch;
    while (true) {
        const ssize_t r = ::recv(fd, &ch, 1, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        if (ch == '\n') break;
        if (ch != '\r') line->push_back(ch);
        if (line->size() > 1024 * 1024) return false;
    }
    return true;
}

bool RecvReply(int fd, std::string* out) {
    out->clear();
    std::string line;
    if (!RecvLine(fd, &line)) return false;
    out->append(line);
    out->push_back('\n');
    if (line.rfind("*PLAN", 0) == 0 || line.rfind("*RESULT", 0) == 0) {
        while (line != "*END") {
            if (!RecvLine(fd, &line)) return false;
            out->append(line);
            out->push_back('\n');
        }
    }
    return true;
}

// Server RecvLine is line-oriented: send each logical line separately.
bool SendStatement(int fd, const std::string& stmt) {
    std::size_t start = 0;
    while (start <= stmt.size()) {
        const std::size_t nl = stmt.find('\n', start);
        std::string line =
            (nl == std::string::npos) ? stmt.substr(start) : stmt.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!SendAll(fd, line + "\n")) return false;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return true;
}

int Connect(const std::string& host, int port, std::string* error) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        *error = std::string("socket() failed: ") + std::strerror(errno);
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        *error = "invalid host address: " + host;
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        *error = std::string("connect() failed: ") + std::strerror(errno);
        return -1;
    }
    return fd;
}

// Map a decoded wire reply to human output. SELECT-style results share
// FormatQueryResult with reldb_sql_shell; OK/error lines are protocol-level.
void PrintDecoded(const reldb::sqlproto::DecodedReply& d) {
    using reldb::sqlproto::ReplyKind;
    switch (d.kind) {
        case ReplyKind::kPong:
            std::cout << "PONG\n";
            break;
        case ReplyKind::kOk:
            if (d.has_rows_affected) {
                std::cout << "rows_affected: " << d.rows_affected << "\n";
            } else if (d.has_in_txn) {
                std::cout << "in_txn: " << (d.in_txn ? "1" : "0") << "\n";
            } else {
                std::cout << "ok\n";
            }
            break;
        case ReplyKind::kError:
            std::cerr << "ERROR: " << d.error_text << "\n";
            break;
        case ReplyKind::kResult:
            reldb::FormatQueryResult(std::cout, d.result);
            break;
        case ReplyKind::kUnknown:
            std::cerr << "ERROR: unknown reply\n";
            break;
    }
}

// Returns false on transport failure. *in_txn updated when reply carries in_txn.
bool ExecAndPrint(int fd, const std::string& text, bool* in_txn) {
    if (!SendStatement(fd, text)) {
        std::cerr << "send failed\n";
        return false;
    }
    std::string raw;
    if (!RecvReply(fd, &raw)) {
        std::cerr << "recv failed (server closed?)\n";
        return false;
    }
    reldb::sqlproto::DecodedReply dec;
    auto st = reldb::sqlproto::DecodeReply(raw, &dec);
    if (!st.ok()) {
        std::cerr << "decode failed: " << st.ToString() << "\nraw:\n" << raw;
        return true;
    }
    PrintDecoded(dec);
    if (in_txn != nullptr && dec.has_in_txn) {
        *in_txn = dec.in_txn;
    }
    return true;
}

// Pull complete statements (first ';' outside quotes) from the front of *buffer.
void ExtractCompleteStatements(std::string* buffer, std::vector<std::string>* out) {
    out->clear();
    while (!buffer->empty()) {
        bool in_string = false;
        std::size_t semi = std::string::npos;
        for (std::size_t i = 0; i < buffer->size(); ++i) {
            const char ch = (*buffer)[i];
            if (in_string) {
                if (ch == '\'') {
                    if (i + 1 < buffer->size() && (*buffer)[i + 1] == '\'') {
                        ++i;
                    } else {
                        in_string = false;
                    }
                }
                continue;
            }
            if (ch == '\'') {
                in_string = true;
                continue;
            }
            if (ch == ';') {
                semi = i;
                break;
            }
        }
        if (semi == std::string::npos) return;

        std::size_t end = semi + 1;
        while (end < buffer->size() &&
               ((*buffer)[end] == ' ' || (*buffer)[end] == '\t' || (*buffer)[end] == '\r')) {
            ++end;
        }
        std::string stmt = buffer->substr(0, end);
        auto tv = reldb::TrimViewWs(stmt);
        if (!tv.empty()) out->push_back(std::string(tv));
        buffer->erase(0, end);
        while (!buffer->empty() && ((*buffer)[0] == '\n' || (*buffer)[0] == '\r')) {
            buffer->erase(0, 1);
        }
    }
}

void PrintHelp() {
    std::cout
        << "Commands:\n"
        << "  SQL ending with ';'     send to server (multi-line ok)\n"
        << "  PING / STATUS / RESET   server meta (single line)\n"
        << "  QUIT                    disconnect (also .quit)\n"
        << "  .help                   this help\n"
        << "  .quit / .exit           disconnect and leave\n"
        << "\n"
        << "For a local embedded DB without TCP, use reldb_sql_shell.\n";
}

bool IsServerMeta(const std::string& trimmed) {
    const std::string u = reldb::ToUpperAscii(trimmed);
    return u == "PING" || u == "STATUS" || u == "RESET" || u == "QUIT";
}

const char* Prompt(bool in_txn, bool cont) {
    if (in_txn) return cont ? "sql'*> " : "sql*> ";
    return cont ? "sql'> " : "sql> ";
}

void BestEffortQuit(int fd) {
    std::string reply;
    (void)SendAll(fd, "QUIT\n");
    (void)RecvReply(fd, &reply);
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 7380;
    std::vector<std::string> batch_chunks;
    bool batch_mode = false;

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
            if (!v) return 2;
            host = v;
        } else if (arg == "--port") {
            const char* v = need("--port");
            if (!v) return 2;
            port = std::atoi(v);
            if (port <= 0 || port > 65535) {
                std::cerr << "invalid port\n";
                return 2;
            }
        } else if (arg == "-c") {
            const char* v = need("-c");
            if (!v) return 2;
            batch_chunks.emplace_back(v);
            batch_mode = true;
        } else if (arg == "--file") {
            const char* v = need("--file");
            if (!v) return 2;
            std::ifstream in(v);
            if (!in) {
                std::cerr << "cannot open file: " << v << "\n";
                return 1;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            batch_chunks.push_back(ss.str());
            batch_mode = true;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            PrintUsage(argv[0]);
            return 2;
        }
    }

    std::string err;
    const int fd = Connect(host, port, &err);
    if (fd < 0) {
        std::cerr << err << "\n";
        return 1;
    }

    bool in_txn = false;
    auto run_complete = [&](std::string* buf) -> bool {
        std::vector<std::string> stmts;
        ExtractCompleteStatements(buf, &stmts);
        for (const auto& s : stmts) {
            if (!ExecAndPrint(fd, s, &in_txn)) return false;
        }
        return true;
    };

    int exit_code = 0;

    if (batch_mode) {
        std::string buffer;
        for (const auto& chunk : batch_chunks) {
            if (!buffer.empty() && buffer.back() != '\n') buffer.push_back('\n');
            buffer += chunk;
        }
        if (!run_complete(&buffer)) {
            exit_code = 1;
        } else if (!reldb::TrimViewWs(buffer).empty()) {
            std::cerr << "ERROR: incomplete statement (missing ';')\n";
            exit_code = 1;
        }
        BestEffortQuit(fd);
        ::close(fd);
        return exit_code;
    }

    std::cout << "reldb_sql_cli  " << host << ":" << port << "\n"
              << "Type .help for help, .quit to exit. End SQL with ';'.\n";

    std::string buffer;
    std::string line;
    std::cout << Prompt(in_txn, false);
    while (std::getline(std::cin, line)) {
        if (!buffer.empty()) {
            buffer.push_back('\n');
            buffer += line;
            if (!reldb::sqlproto::EndsWithStatementTerminator(buffer)) {
                std::cout << Prompt(in_txn, true);
                continue;
            }
            if (!run_complete(&buffer)) {
                exit_code = 1;
                break;
            }
            std::cout << Prompt(in_txn, false);
            continue;
        }

        const std::string trimmed(reldb::TrimViewWs(line));
        if (trimmed.empty()) {
            std::cout << Prompt(in_txn, false);
            continue;
        }

        if (trimmed[0] == '.') {
            if (trimmed == ".help" || trimmed == ".h" || trimmed == "?") {
                PrintHelp();
            } else if (trimmed == ".quit" || trimmed == ".exit" || trimmed == ".q") {
                break;
            } else {
                std::cerr << "unknown meta-command: " << trimmed << " (try .help)\n";
            }
            std::cout << Prompt(in_txn, false);
            continue;
        }

        if (IsServerMeta(trimmed)) {
            const std::string u = reldb::ToUpperAscii(trimmed);
            if (!ExecAndPrint(fd, u, &in_txn)) {
                exit_code = 1;
                break;
            }
            if (u == "QUIT") {
                ::close(fd);
                return 0;
            }
            // Refresh txn flag after non-STATUS commands with an optional STATUS.
            if (u != "STATUS") {
                std::string raw;
                if (SendStatement(fd, "STATUS") && RecvReply(fd, &raw)) {
                    reldb::sqlproto::DecodedReply dec;
                    if (reldb::sqlproto::DecodeReply(raw, &dec).ok() && dec.has_in_txn) {
                        in_txn = dec.in_txn;
                    }
                }
            }
            std::cout << Prompt(in_txn, false);
            continue;
        }

        buffer = line;
        if (!reldb::sqlproto::EndsWithStatementTerminator(buffer)) {
            std::cout << Prompt(in_txn, true);
            continue;
        }
        if (!run_complete(&buffer)) {
            exit_code = 1;
            break;
        }
        // Keep in_txn accurate after SQL (BEGIN/COMMIT/ABORT).
        {
            std::string raw;
            if (SendStatement(fd, "STATUS") && RecvReply(fd, &raw)) {
                reldb::sqlproto::DecodedReply dec;
                if (reldb::sqlproto::DecodeReply(raw, &dec).ok() && dec.has_in_txn) {
                    in_txn = dec.in_txn;
                }
            }
        }
        std::cout << Prompt(in_txn, false);
    }

    BestEffortQuit(fd);
    ::close(fd);
    return exit_code;
}
