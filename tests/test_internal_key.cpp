#include "test_harness.h"
#include "lsmkv/encoding.h"
#include "lsmkv/internal_key.h"

TEST(internal_key_roundtrip) {
    auto ik = lsmkv::MakeInternalKey("apple", 42, lsmkv::kTypeValue);
    expect_eq(lsmkv::ExtractUserKey(ik).ToString(), std::string("apple"), "user");
    expect_eq(lsmkv::ExtractSequence(ik), 42ull, "seq");
    expect(lsmkv::ExtractValueType(ik) == lsmkv::kTypeValue, "type");
}

TEST(internal_key_deletion_roundtrip) {
    auto ik = lsmkv::MakeInternalKey("gone", 7, lsmkv::kTypeDeletion);
    expect_eq(lsmkv::ExtractUserKey(ik).ToString(), std::string("gone"), "user");
    expect_eq(lsmkv::ExtractSequence(ik), 7ull, "seq");
    expect(lsmkv::ExtractValueType(ik) == lsmkv::kTypeDeletion, "deletion type");
}

TEST(internal_key_pack_helpers) {
    auto packed_value = lsmkv::PackSequenceAndType(99, lsmkv::kTypeValue);
    expect_eq(lsmkv::SequenceFromPack(packed_value), 99ull, "seq from pack");
    expect(lsmkv::TypeFromPack(packed_value) == lsmkv::kTypeValue, "type value from pack");

    auto packed_del = lsmkv::PackSequenceAndType(99, lsmkv::kTypeDeletion);
    expect_eq(lsmkv::SequenceFromPack(packed_del), 99ull, "seq preserved");
    expect(lsmkv::TypeFromPack(packed_del) == lsmkv::kTypeDeletion, "type deletion from pack");
    expect(packed_value != packed_del, "type affects pack");
    expect(packed_value > packed_del, "value type sorts above deletion at same seq");
}

TEST(internal_key_ordering) {
    auto a1 = lsmkv::MakeInternalKey("a", 1, lsmkv::kTypeValue);
    auto a2 = lsmkv::MakeInternalKey("a", 2, lsmkv::kTypeValue);
    auto b1 = lsmkv::MakeInternalKey("b", 1, lsmkv::kTypeValue);
    expect(lsmkv::CompareInternalKey(a2, a1) < 0, "newer first");
    expect(lsmkv::CompareInternalKey(a1, b1) < 0, "user asc");
    expect(lsmkv::CompareInternalKey(a1, a1) == 0, "equal keys");
}

TEST(internal_key_type_tiebreak) {
    auto del = lsmkv::MakeInternalKey("k", 5, lsmkv::kTypeDeletion);
    auto val = lsmkv::MakeInternalKey("k", 5, lsmkv::kTypeValue);
    // Larger packed trailer sorts first, so value precedes deletion at equal seq.
    expect(lsmkv::CompareInternalKey(val, del) < 0, "value before deletion at same seq");
}

TEST(internal_key_lookup_key) {
    auto lookup = lsmkv::MakeLookupKey("user", 10);
    expect_eq(lsmkv::ExtractUserKey(lookup).ToString(), std::string("user"), "lookup user");
    expect_eq(lsmkv::ExtractSequence(lookup), 10ull, "lookup seq");
    expect(lsmkv::ExtractValueType(lookup) == lsmkv::kTypeValue, "lookup uses value type");

    auto older_value = lsmkv::MakeInternalKey("user", 9, lsmkv::kTypeValue);
    auto newer_value = lsmkv::MakeInternalKey("user", 11, lsmkv::kTypeValue);
    expect(lsmkv::CompareInternalKey(lookup, older_value) < 0, "lookup before older seq");
    expect(lsmkv::CompareInternalKey(newer_value, lookup) < 0, "newer seq before lookup");
}

TEST(internal_key_empty_user_key) {
    auto ik = lsmkv::MakeInternalKey("", 1, lsmkv::kTypeValue);
    expect(lsmkv::ExtractUserKey(ik).empty(), "empty user");
    expect_eq(ik.size(), std::size_t{8}, "trailer only");
    expect_eq(lsmkv::ExtractSequence(ik), 1ull, "seq");
}

TEST(internal_key_short_slice_fallback) {
    lsmkv::Slice short_key("abc");
    expect_eq(lsmkv::ExtractUserKey(short_key).ToString(), std::string("abc"), "short user passthrough");
    expect_eq(lsmkv::ExtractSequence(short_key), 0ull, "short seq default");
    expect(lsmkv::ExtractValueType(short_key) == lsmkv::kTypeValue, "short type default");
    expect(lsmkv::CompareInternalKey(short_key, lsmkv::Slice("abd")) < 0, "short compare memcmp");
}

TEST(internal_key_comparator_functor) {
    lsmkv::InternalKeyComparator cmp;
    auto a = lsmkv::MakeInternalKey("a", 2, lsmkv::kTypeValue);
    auto b = lsmkv::MakeInternalKey("a", 1, lsmkv::kTypeValue);
    expect(cmp(a, b) < 0, "functor newer first");
    expect(cmp(b, a) > 0, "functor older second");
    expect(cmp(a, a) == 0, "functor equal");
}
