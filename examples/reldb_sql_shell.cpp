// Interactive SQL shell over reldb::SqlSession.
//
//   reldb_sql_shell [--db PATH]
//
// Type SQL terminated by ';'. Meta-commands: .help, .quit
// Ctrl-C (SIGINT) exits the shell cleanly.
// Up/down arrows recall the last 50 completed statements (GNU Readline).
//
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <readline/history.h>
#include <readline/readline.h>

#include "lsmkv/options.h"
#include "lsmkv/status.h"
#include "reldb/database.h"
#include "reldb/query_format.h"
#include "reldb/query_result.h"
#include "reldb/sql_session.h"
#include "reldb/types.h"

namespace {

constexpr int kMaxHistory = 50;

// Set by SIGINT handler; checked in the main loop. sig_atomic_t is async-signal-safe.
volatile std::sig_atomic_t g_got_sigint = 0;

void OnSigInt(int /*signo*/) { g_got_sigint = 1; }

void PrintUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [--db PATH]\n"
              << "  Interactive SQL shell for reldb.\n"
              << "  --db PATH   database directory (default: /tmp/reldb_sql_shell)\n";
}

void PrintHelp() {
    std::cout
        << "Commands:\n"
        << "  SQL statement ending with ';'   execute (multi-line ok)\n"
        << "  Up/Down arrows                  previous/next statement (last " << kMaxHistory
        << ")\n"
        << "  .help                           this help\n"
        << "  .quit / .exit / Ctrl-D          leave the shell\n"
        << "  Ctrl-C                          leave the shell\n"
        << "\n"
        << "SQL (subset): CREATE TABLE, INSERT, SELECT, UPDATE, DELETE,\n"
        << "  BEGIN, COMMIT, ABORT/ROLLBACK.\n"
        << "DDL is not allowed inside a transaction.\n"
        << "Without BEGIN, DML/SELECT auto-commit.\n";
}

void PrintResult(const reldb::QueryResult& r) {
    reldb::FormatQueryResult(std::cout, r);
}

// True if s ends a complete statement: a ';' not inside a single-quoted string.
bool EndsWithStatementTerminator(const std::string& s) {
    bool in_string = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (in_string) {
            if (ch == '\'') {
                if (i + 1 < s.size() && s[i + 1] == '\'') {
                    ++i;  // escaped quote
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
            // Allow trailing whitespace after last ';'
            std::size_t j = i + 1;
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) {
                ++j;
            }
            if (j == s.size()) return true;
        }
    }
    return false;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' ||
                          s.back() == '\r')) {
        s.pop_back();
    }
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
    return s.substr(i);
}

bool IsMeta(const std::string& line) {
    return !line.empty() && line[0] == '.';
}

// Append a completed SQL statement to history (cap kMaxHistory, skip exact dups of last).
void RememberStatement(const std::string& sql) {
    const std::string t = Trim(sql);
    if (t.empty()) return;
    if (history_length > 0) {
        HIST_ENTRY* last = history_get(history_base + history_length - 1);
        if (last != nullptr && last->line != nullptr && t == last->line) return;
    }
    add_history(t.c_str());
}

// Read one input line with readline (Up/Down = history). Empty optional means EOF/SIGINT.
bool ReadLine(const char* prompt, std::string* out) {
    char* raw = readline(prompt);
    if (raw == nullptr) {
        // EOF or interrupted.
        return false;
    }
    *out = raw;
    std::free(raw);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string db_path = "/tmp/reldb_sql_shell";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown argument: " << argv[i] << "\n";
            PrintUsage(argv[0]);
            return 2;
        }
    }

    lsmkv::Options opt;
    opt.create_if_missing = true;
    std::shared_ptr<reldb::Database> db;
    auto st = reldb::Database::Open(opt, db_path, &db);
    if (!st.ok()) {
        std::cerr << "open " << db_path << ": " << st.ToString() << "\n";
        return 1;
    }

    // Keep our SIGINT handler; do not let readline install its own.
    rl_catch_signals = 0;
    using_history();
    stifle_history(kMaxHistory);

    // Install SIGINT handler so Ctrl-C exits cleanly instead of aborting mid-flight.
    // The default handler would kill the process without running SqlSession cleanup;
    // with this handler, readline is interrupted and we fall through to a normal exit.
    std::signal(SIGINT, OnSigInt);

    reldb::SqlSession session(db);
    std::cout << "reldb SQL shell  db=" << db_path << "\n"
              << "Type .help for help, .quit or Ctrl-C to exit. End SQL with ';'.\n"
              << "Up/Down arrows recall the last " << kMaxHistory << " statements.\n";

    std::string buffer;
    std::string line;
    while (!g_got_sigint) {
        const bool cont = !buffer.empty();
        const char* prompt = session.InTransaction() ? (cont ? "sql'*> " : "sql*> ")
                                                     : (cont ? "sql'> " : "sql> ");
        if (!ReadLine(prompt, &line)) {
            // EOF (Ctrl-D) or interrupted by SIGINT.
            std::cout << "\n";
            break;
        }
        if (g_got_sigint) {
            std::cout << "\n";
            break;
        }

        if (!cont) {
            const std::string trimmed = Trim(line);
            if (trimmed.empty()) continue;
            if (IsMeta(trimmed)) {
                if (trimmed == ".help" || trimmed == ".h" || trimmed == "?") {
                    PrintHelp();
                } else if (trimmed == ".quit" || trimmed == ".exit" || trimmed == ".q") {
                    break;
                } else {
                    std::cerr << "unknown meta-command: " << trimmed << " (try .help)\n";
                }
                continue;
            }
        }

        if (!buffer.empty()) buffer.push_back('\n');
        buffer += line;

        if (!EndsWithStatementTerminator(buffer)) continue;

        RememberStatement(buffer);

        reldb::QueryResult result;
        st = session.Execute(buffer, result);
        buffer.clear();
        // Prefer reporting the statement result before honoring Ctrl-C so the
        // user still sees ERROR / rows if Execute finished (or failed) first.
        if (!st.ok()) {
            std::cerr << "ERROR: " << st.ToString() << "\n";
        } else {
            PrintResult(result);
        }
        if (g_got_sigint) {
            std::cout << "\n";
            break;
        }
    }

    if (g_got_sigint) {
        std::cerr << "Interrupted.\n";
    }
    if (session.InTransaction()) {
        std::cerr << "warning: open transaction left uncommitted (will abort on exit)\n";
    }
    return 0;
}
