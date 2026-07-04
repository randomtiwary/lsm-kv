#pragma once

#include <cstdint>
#include <string>

#include "lsmkv/status.h"

namespace lsmkv {

// Small filesystem helpers shared by VersionSet, DB open/recovery, and tests.
// Keeps POSIX stat/mkdir/rename and basic file IO out of higher-level modules.

bool PathExists(const std::string& path);
bool DirExists(const std::string& path);

// Create a directory. Returns OK if it already exists as a directory.
Status CreateDir(const std::string& path);
Status RenameFile(const std::string& src, const std::string& target);

// Overwrite `path` with `data` (truncating).
Status WriteStringToFile(const std::string& data, const std::string& path);
// Write to path + ".tmp", then rename into place (for CURRENT-style updates).
Status WriteStringToFileAtomic(const std::string& data, const std::string& path);
// Append bytes to an existing file (created if missing).
Status AppendStringToFile(const std::string& data, const std::string& path);
Status ReadFileToString(const std::string& path, std::string* data);

// Final path component; if `path` has no '/', returns `path` unchanged.
std::string Basename(const std::string& path);

// Standard DB filename layout under `dbname/`.
std::string TableFileName(const std::string& dbname, std::uint64_t number);
std::string LogFileName(const std::string& dbname, std::uint64_t number);
std::string ManifestFileName(const std::string& dbname, std::uint64_t number);
std::string CurrentFileName(const std::string& dbname);
std::string LockFileName(const std::string& dbname);

}  // namespace lsmkv
