
#include "test_harness.h"
#include "test_util.h"
#include "lsmkv/env.h"

TEST(env_dir_and_file_io) {
    auto dir = MakeTempDir("lsmkv_env");
    expect(lsmkv::CreateDir(dir).ok(), "create existing ok");
    expect(lsmkv::DirExists(dir), "dir exists");
    expect(lsmkv::PathExists(dir), "path exists");

    std::string path = dir + "/blob";
    expect(lsmkv::WriteStringToFile("hello", path).ok(), "write");
    std::string data;
    expect(lsmkv::ReadFileToString(path, &data).ok(), "read");
    expect_eq(data, std::string("hello"), "content");
    expect(lsmkv::AppendStringToFile("!", path).ok(), "append");
    expect(lsmkv::ReadFileToString(path, &data).ok(), "read2");
    expect_eq(data, std::string("hello!"), "appended");

    std::string atomic_path = dir + "/CURRENT";
    expect(lsmkv::WriteStringToFileAtomic("MANIFEST-1\n", atomic_path).ok(), "atomic");
    expect(lsmkv::ReadFileToString(atomic_path, &data).ok(), "read current");
    expect_eq(data, std::string("MANIFEST-1\n"), "current contents");
    expect(!lsmkv::PathExists(atomic_path + ".tmp"), "tmp removed");

    expect_eq(lsmkv::Basename("/tmp/x/MANIFEST-2"), std::string("MANIFEST-2"), "basename");
    expect_eq(lsmkv::TableFileName(dir, 7), dir + "/7.sst", "table name");
    expect_eq(lsmkv::LogFileName(dir, 3), dir + "/3.log", "log name");
    expect_eq(lsmkv::ManifestFileName(dir, 1), dir + "/MANIFEST-1", "manifest name");
    expect_eq(lsmkv::CurrentFileName(dir), dir + "/CURRENT", "current name");
    RemoveDirRecursive(dir);
}
