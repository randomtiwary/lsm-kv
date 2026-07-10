#pragma once

#include <cstdint>
#include <string>

#include "lsmkv/common.h"
#include "lsmkv/slice.h"

namespace lsmkv {

enum ValueType : std::uint8_t {
    kTypeDeletion = 0x0,
    kTypeValue = 0x1
};

// Packed sequence (56 bits) and type (8 bits).
inline std::uint64_t PackSequenceAndType(Timestamp seq, ValueType t) {
    return (seq << 8) | static_cast<std::uint64_t>(t);
}
inline Timestamp SequenceFromPack(std::uint64_t packed) { return packed >> 8; }
inline ValueType TypeFromPack(std::uint64_t packed) {
    return static_cast<ValueType>(packed & 0xFF);
}

// internal_key = user_key + fixed64(pack(seq, type))
std::string MakeInternalKey(const Slice& user_key, Timestamp seq, ValueType t);
Slice ExtractUserKey(const Slice& internal_key);
Timestamp ExtractSequence(const Slice& internal_key);
ValueType ExtractValueType(const Slice& internal_key);

// Order: ascending user key, then descending sequence, then descending type.
int CompareInternalKey(const Slice& a, const Slice& b);

struct InternalKeyComparator {
    int operator()(const std::string& a, const std::string& b) const {
        return CompareInternalKey(a, b);
    }
};

// Lookup key used for MemTable Get: user_key + pack(seq=max, type=Value)
std::string MakeLookupKey(const Slice& user_key, Timestamp sequence);

}  // namespace lsmkv
