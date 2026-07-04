#pragma once
#include <vector>

#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

inline std::string MakeTempDir(const std::string& prefix) {
    std::string path = "/tmp/" + prefix + "_XXXXXX";
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');
    char* result = mkdtemp(buf.data());
    if (result == nullptr) return "/tmp/" + prefix + "_fallback";
    return std::string(result);
}

inline void RemoveDirRecursive(const std::string& path) {
    std::string cmd = "rm -rf '" + path + "'";
    std::system(cmd.c_str());
}
