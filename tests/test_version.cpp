
#include "test_harness.h"
#include "test_util.h"
#include "lsmkv/internal_key.h"
#include "lsmkv/version.h"

TEST(version_edit_roundtrip) {
    lsmkv::VersionEdit e;
    e.SetNextFile(10);
    e.SetLastSequence(99);
    e.SetLogNumber(3);
    lsmkv::FileMetaData f;
    f.number = 7;
    f.file_size = 123;
    f.smallest = lsmkv::MakeInternalKey("a", 1, lsmkv::kTypeValue);
    f.largest = lsmkv::MakeInternalKey("z", 1, lsmkv::kTypeValue);
    e.AddFile(0, f);
    e.DeleteFile(1, 4);
    auto enc = e.Encode();
    lsmkv::VersionEdit d;
    expect(d.DecodeFrom(enc).ok(), "decode");
    expect(d.has_next_file_number && d.next_file_number == 10, "next");
    expect(d.has_last_sequence && d.last_sequence == 99, "seq");
    expect(d.added_files.size() == 1 && d.added_files[0].second.number == 7, "add");
    expect(d.deleted_files.size() == 1 && d.deleted_files[0].second == 4, "del");
}

TEST(version_set_new_recover) {
    auto dir = MakeTempDir("lsmkv_ver");
    lsmkv::VersionSet vs(dir);
    expect(vs.NewDB().ok(), "new");
    lsmkv::FileMetaData f;
    f.number = vs.NextFileNumber();
    f.file_size = 1;
    f.smallest = lsmkv::MakeInternalKey("a", 1, lsmkv::kTypeValue);
    f.largest = lsmkv::MakeInternalKey("b", 1, lsmkv::kTypeValue);
    lsmkv::VersionEdit e;
    e.AddFile(0, f);
    e.SetLastSequence(5);
    expect(vs.LogAndApply(&e).ok(), "apply");
    lsmkv::VersionSet vs2(dir);
    expect(vs2.Recover().ok(), "recover");
    expect(vs2.current()->NumLevelFiles(0) == 1, "one file");
    expect_eq(vs2.LastSequence(), 5ull, "seq");
    RemoveDirRecursive(dir);
}
