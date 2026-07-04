#include "lsmkv/crc32.h"

namespace lsmkv {
namespace {

// Precomputed CRC-32 remainder for each byte value under polynomial 0xEDB88320.
// Built once on first use; indexing by (crc ^ byte) & 0xFF folds one input byte
// into the running register per iteration in Crc32Extend.
std::uint32_t table[256];
bool table_init = false;

void InitTable() {
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            // LSB-first (reflected) division: XOR the poly when the low bit is set.
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    table_init = true;
}

}  // namespace

std::uint32_t Crc32Extend(std::uint32_t crc, const char* data, std::size_t n) {
    if (!table_init) InitTable();
    // Invert around the loop so incremental Extend calls compose cleanly with
    // the standard init/final XOR of 0xFFFFFFFF.
    crc = crc ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        crc = table[(crc ^ static_cast<unsigned char>(data[i])) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t Crc32(const char* data, std::size_t n) {
    return Crc32Extend(0, data, n);
}

}  // namespace lsmkv
