#pragma once

#include <cstdint>
#include <cstddef>

namespace lsmkv {

// CRC-32 checksums (ISO 3309 / ITU-T V.42 / Ethernet / ZIP polynomial).
//
// Used by the WAL (and later SSTables) to detect torn or bit-flipped records.
// Algorithm details:
//   - Polynomial: 0xEDB88320 (bit-reflected form of 0x04C11DB7)
//   - Init/final XOR: 0xFFFFFFFF (so the checksum of empty input is 0)
//   - Byte-at-a-time table lookup implemented in crc32.cpp
//
// Crc32(data, n) hashes a single contiguous buffer from a fresh initial state.
// Crc32Extend(crc, data, n) continues a running checksum: hashing A then B with
// Extend is identical to hashing A||B with Crc32 in one shot. That lets callers
// stream bytes without buffering the whole payload.
//
// On-disk storage always uses MaskCrc(Crc32(payload)), never the raw value.
// See MaskCrc / UnmaskCrc below.

std::uint32_t Crc32(const char* data, std::size_t n);
std::uint32_t Crc32Extend(std::uint32_t crc, const char* data, std::size_t n);

// LevelDB-style CRC masking.
//
// A raw CRC32 of a string of zeros is itself zero, so an all-zero 8-byte header
// (length=0, crc=0) would look like a valid empty record and could also appear
// inside payload data. Masking rotates the checksum and adds a constant so that:
//   - embedded CRCs are very unlikely to collide with a plausible header, and
//   - the transform is invertible via UnmaskCrc for verification.
//
//   MaskCrc(c)   = rotate_right(c, 15) + 0xA282EAD8
//   UnmaskCrc(m) = rotate_left(m - 0xA282EAD8, 15)
//
// WAL writers store MaskCrc(Crc32(payload)); readers recompute Crc32(payload)
// and compare MaskCrc(recomputed) against the stored word (or unmask first).
inline std::uint32_t MaskCrc(std::uint32_t crc) {
    return ((crc >> 15) | (crc << 17)) + 0xA282EAD8u;
}
inline std::uint32_t UnmaskCrc(std::uint32_t masked) {
    std::uint32_t rot = masked - 0xA282EAD8u;
    return (rot >> 17) | (rot << 15);
}

}  // namespace lsmkv
