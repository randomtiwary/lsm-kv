// TCP SQL server entry point (reldb_sql_server).
//
// Line-oriented protocol (see docs/ROADMAP_SERVER_DDL_AGG_JOIN.md):
//   Meta: PING | QUIT | RESET | STATUS
//   SQL: multi-line until ';' outside quotes
//
// Usage:
//   reldb_sql_server [--port PORT] [--host HOST] [--db PATH] [--max-clients N]
// Defaults: host=127.0.0.1, port=7380, db=./reldb_sql_data, max-clients=128

#include <signal.h>

#include <iostream>
#include <string>

#include "sql_server.h"

namespace {
reldb::SqlServer* g_server = nullptr;

void OnSignal(int /*sig*/) {
    if (g_server) g_server->Stop();
}
}  // namespace

int main(int argc, char** argv) {
    reldb::SqlServerConfig cfg;
    std::string error;
    if (!reldb::ParseSqlServerArgs(argc, argv, &cfg, &error)) {
        std::cerr << error << "\n";
        reldb::PrintSqlServerUsage(argv[0]);
        return 2;
    }

    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, OnSignal);
    ::signal(SIGTERM, OnSignal);

    reldb::SqlServer server(cfg);
    g_server = &server;

    lsmkv::Status s = server.Start();
    if (!s.ok()) {
        std::cerr << "start failed: " << s.ToString() << "\n";
        g_server = nullptr;
        return 1;
    }

    std::cout << "reldb_sql_server listening on " << cfg.host << ":" << server.port()
              << "  db=" << cfg.db_path << "  max_clients=" << cfg.max_clients << std::endl;

    server.Serve();
    g_server = nullptr;
    std::cout << "reldb_sql_server shut down" << std::endl;
    return 0;
}
