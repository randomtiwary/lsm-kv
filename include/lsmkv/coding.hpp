#pragma once

#include <cstdint>
#include <string>

#include "lsmkv/slice.hpp"

namespace lsmkv {

void PutFixed32(std::string* dst, std::uint32_t value);
void PutFixed64(std::string* dst, std::uint64_t value);
void PutVarint32(std::string* dst, std::uint32_t value);
void PutVarint64(std::string* dst, std::uint64_t value);
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);

std::uint32_t DecodeFixed32(const char* ptr);
std::uint64_t DecodeFixed64(const char* ptr);

bool GetFixed32(Slice* input, std::uint32_t* value);
bool GetFixed64(Slice* input, std::uint64_t* value);
bool GetVarint32(Slice* input, std::uint32_t* value);
bool GetVarint64(Slice* input, std::uint64_t* value);
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

const char* GetVarint32Ptr(const char* p, const char* limit, std::uint32_t* value);
const char* GetVarint64Ptr(const char* p, const char* limit, std::uint64_t* value);

int VarintLength(std::uint64_t v);

}  // namespace lsmkv
