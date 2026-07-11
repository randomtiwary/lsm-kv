#include "lsmkv/internal_key.h"
#include <cassert>

#include "lsmkv/encoding.h"

namespace lsmkv {

std::string MakeInternalKey(const Slice& user_key, Timestamp seq, ValueType t) {
    std::string key;
    key.reserve(user_key.size() + 8);
    key.append(user_key.data(), user_key.size());
    PutFixed64(&key, PackSequenceAndType(seq, t));
    return key;
}

Slice ExtractUserKey(const Slice& internal_key) {
    if (internal_key.size() < 8) return internal_key;
    return Slice(internal_key.data(), internal_key.size() - 8);
}

Timestamp ExtractSequence(const Slice& internal_key) {
    if (internal_key.size() < 8) return 0;
    std::uint64_t packed = DecodeFixed64(internal_key.data() + internal_key.size() - 8);
    return SequenceFromPack(packed);
}

ValueType ExtractValueType(const Slice& internal_key) {
    if (internal_key.size() < 8) return kTypeValue;
    std::uint64_t packed = DecodeFixed64(internal_key.data() + internal_key.size() - 8);
    return TypeFromPack(packed);
}

int CompareInternalKey(const Slice& a, const Slice& b) {
    // Fallback to memcmp ordering for non-internal keys (e.g. unit tests on raw blocks).
    if (a.size() < 8 || b.size() < 8) return a.compare(b);
    Slice au = ExtractUserKey(a);
    Slice bu = ExtractUserKey(b);
    int r = au.compare(bu);
    if (r != 0) return r;
    std::uint64_t ap = DecodeFixed64(a.data() + a.size() - 8);
    std::uint64_t bp = DecodeFixed64(b.data() + b.size() - 8);
    if (ap > bp) return -1;  // larger sequence first
    if (ap < bp) return 1;
    return 0;
}

std::string MakeLookupKey(const Slice& user_key, Timestamp sequence) {
    return MakeInternalKey(user_key, sequence, kTypeValue);
}

}  // namespace lsmkv
