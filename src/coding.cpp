#include "lsmkv/coding.hpp"

#include <cstring>

namespace lsmkv {

void PutFixed32(std::string* dst, std::uint32_t value) {
    char buf[4];
    std::memcpy(buf, &value, sizeof(value));
    dst->append(buf, 4);
}

void PutFixed64(std::string* dst, std::uint64_t value) {
    char buf[8];
    std::memcpy(buf, &value, sizeof(value));
    dst->append(buf, 8);
}

void PutVarint32(std::string* dst, std::uint32_t v) {
    char buf[5];
    char* ptr = buf;
    while (v >= 0x80) {
        *(ptr++) = static_cast<char>(v | 0x80);
        v >>= 7;
    }
    *(ptr++) = static_cast<char>(v);
    dst->append(buf, static_cast<std::size_t>(ptr - buf));
}

void PutVarint64(std::string* dst, std::uint64_t v) {
    char buf[10];
    char* ptr = buf;
    while (v >= 0x80) {
        *(ptr++) = static_cast<char>(v | 0x80);
        v >>= 7;
    }
    *(ptr++) = static_cast<char>(v);
    dst->append(buf, static_cast<std::size_t>(ptr - buf));
}

void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
    PutVarint32(dst, static_cast<std::uint32_t>(value.size()));
    dst->append(value.data(), value.size());
}

std::uint32_t DecodeFixed32(const char* ptr) {
    std::uint32_t result;
    std::memcpy(&result, ptr, sizeof(result));
    return result;
}

std::uint64_t DecodeFixed64(const char* ptr) {
    std::uint64_t result;
    std::memcpy(&result, ptr, sizeof(result));
    return result;
}

bool GetFixed32(Slice* input, std::uint32_t* value) {
    if (input->size() < 4) return false;
    *value = DecodeFixed32(input->data());
    input->remove_prefix(4);
    return true;
}

bool GetFixed64(Slice* input, std::uint64_t* value) {
    if (input->size() < 8) return false;
    *value = DecodeFixed64(input->data());
    input->remove_prefix(8);
    return true;
}

const char* GetVarint32Ptr(const char* p, const char* limit, std::uint32_t* value) {
    std::uint32_t result = 0;
    for (std::uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
        std::uint32_t byte = static_cast<unsigned char>(*p);
        ++p;
        if (byte & 0x80) {
            result |= ((byte & 0x7F) << shift);
        } else {
            result |= (byte << shift);
            *value = result;
            return p;
        }
    }
    return nullptr;
}

const char* GetVarint64Ptr(const char* p, const char* limit, std::uint64_t* value) {
    std::uint64_t result = 0;
    for (std::uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
        std::uint64_t byte = static_cast<unsigned char>(*p);
        ++p;
        if (byte & 0x80) {
            result |= ((byte & 0x7F) << shift);
        } else {
            result |= (byte << shift);
            *value = result;
            return p;
        }
    }
    return nullptr;
}

bool GetVarint32(Slice* input, std::uint32_t* value) {
    const char* p = input->data();
    const char* limit = p + input->size();
    const char* q = GetVarint32Ptr(p, limit, value);
    if (q == nullptr) return false;
    input->remove_prefix(static_cast<std::size_t>(q - p));
    return true;
}

bool GetVarint64(Slice* input, std::uint64_t* value) {
    const char* p = input->data();
    const char* limit = p + input->size();
    const char* q = GetVarint64Ptr(p, limit, value);
    if (q == nullptr) return false;
    input->remove_prefix(static_cast<std::size_t>(q - p));
    return true;
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
    std::uint32_t len = 0;
    if (!GetVarint32(input, &len) || input->size() < len) return false;
    *result = Slice(input->data(), len);
    input->remove_prefix(len);
    return true;
}

int VarintLength(std::uint64_t v) {
    int len = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++len;
    }
    return len;
}

}  // namespace lsmkv
