#include "lsmkv/env.h"

#include <fstream>
#include <sys/stat.h>

namespace lsmkv {

bool PathExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool DirExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

Status CreateDir(const std::string& path) {
    if (DirExists(path)) return STATUS(OK);
    if (::mkdir(path.c_str(), 0755) != 0) {
        if (DirExists(path)) return STATUS(OK);
        return STATUS(IOError, "mkdir failed: " + path);
    }
    return STATUS(OK);
}

Status RenameFile(const std::string& src, const std::string& target) {
    if (std::rename(src.c_str(), target.c_str()) != 0) {
        return STATUS(IOError, "rename failed: " + src + " -> " + target);
    }
    return STATUS(OK);
}

Status WriteStringToFile(const std::string& data, const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return STATUS(IOError, "cannot write file: " + path);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) return STATUS(IOError, "write failed: " + path);
    return STATUS(OK);
}

Status WriteStringToFileAtomic(const std::string& data, const std::string& path) {
    const std::string tmp = path + ".tmp";
    Status s = WriteStringToFile(data, tmp);
    if (!s.ok()) return s;
    return RenameFile(tmp, path);
}

Status AppendStringToFile(const std::string& data, const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return STATUS(IOError, "cannot append file: " + path);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) return STATUS(IOError, "append failed: " + path);
    return STATUS(OK);
}

Status ReadFileToString(const std::string& path, std::string* data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return STATUS(IOError, "cannot read file: " + path);
    in.seekg(0, std::ios::end);
    const auto n = in.tellg();
    if (n < 0) return STATUS(IOError, "stat failed: " + path);
    in.seekg(0);
    data->assign(static_cast<std::size_t>(n), '\0');
    if (n > 0) {
        in.read(&(*data)[0], n);
        if (!in) return STATUS(IOError, "read failed: " + path);
    }
    return STATUS(OK);
}

std::string Basename(const std::string& path) {
    const auto pos = path.find_last_of('/');
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

std::string TableFileName(const std::string& dbname, std::uint64_t number) {
    return dbname + "/" + std::to_string(number) + ".sst";
}

std::string LogFileName(const std::string& dbname, std::uint64_t number) {
    return dbname + "/" + std::to_string(number) + ".log";
}

std::string ManifestFileName(const std::string& dbname, std::uint64_t number) {
    return dbname + "/MANIFEST-" + std::to_string(number);
}

std::string CurrentFileName(const std::string& dbname) { return dbname + "/CURRENT"; }

std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

}  // namespace lsmkv
