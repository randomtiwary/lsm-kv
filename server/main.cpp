// TCP server entry point for the lsm-kv embedded engine.
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
//   NOT_FOUND          // key not found
//   -ERR <message>
//
// Usage:
//   lsmkv_server [--port PORT] [--host HOST] [--db PATH] [--max-clients N]
// Defaults: host=0.0.0.0, port=7379, db=./lsmkv_data, max-clients=128

#include <signal.h>

#include <iostream>
#include <string>

#include "server.h"

namespace {
lsmkv::Server* g_server = nullptr;

void OnSignal(int /*sig*/) {
    if (g_server) g_server->Stop();
}
}  // namespace

int main(int argc, char** argv) {
    lsmkv::ServerConfig cfg;
    std::string error;
    if (!lsmkv::ParseServerArgs(argc, argv, &cfg, &error)) {
        std::cerr << error << "\n";
        lsmkv::PrintServerUsage(argv[0]);
        return 2;
    }

    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, OnSignal);
    ::signal(SIGTERM, OnSignal);

    lsmkv::Server server(cfg);
    g_server = &server;

    lsmkv::Status s = server.Start();
    if (!s.ok()) {
        std::cerr << "start failed: " << s.ToString() << "\n";
        g_server = nullptr;
        return 1;
    }

    std::cout << "lsmkv_server listening on " << cfg.host << ":" << server.port()
              << "  db=" << cfg.db_path
              << "  max_clients=" << cfg.max_clients << std::endl;

    server.Serve();
    g_server = nullptr;
    std::cout << "lsmkv_server shut down" << std::endl;
    return 0;
}
