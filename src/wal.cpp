#include "lsmkv/wal.h"

#include "lsmkv/encoding.h"
#include "lsmkv/crc32.h"

namespace lsmkv {

WalWriter::WalWriter(std::string path) : path_(std::move(path)) {}
WalWriter::~WalWriter() { (void)Close(); }

Status WalWriter::Open() {
    out_.open(path_, std::ios::binary | std::ios::app);
    if (!out_) return STATUS(IOError, "cannot open wal for append: " + path_);
    open_ = true;
    return STATUS(OK);
}

Status WalWriter::AddRecord(const Slice& data) {
    if (!open_) return STATUS(IOError, "wal not open");
    // Frame: [fixed32 length][fixed32 masked_crc][payload bytes].
    // CRC is computed over `data` only, then masked before storage.
    std::uint32_t len = static_cast<std::uint32_t>(data.size());
    std::uint32_t crc = MaskCrc(Crc32(data.data(), data.size()));
    std::string header;
    PutFixed32(&header, len);
    PutFixed32(&header, crc);
    out_.write(header.data(), static_cast<std::streamsize>(header.size()));
    out_.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out_) return STATUS(IOError, "wal write failed");
    return STATUS(OK);
}

Status WalWriter::Sync() {
    if (!open_) return STATUS(IOError, "wal not open");
    out_.flush();
    if (!out_) return STATUS(IOError, "wal flush failed");
    return STATUS(OK);
}

Status WalWriter::Close() {
    if (!open_) return STATUS(OK);
    out_.flush();
    out_.close();
    open_ = false;
    return STATUS(OK);
}

WalReader::WalReader(std::string path) : path_(std::move(path)) {}

Status WalReader::Open() {
    in_.open(path_, std::ios::binary);
    if (!in_) return STATUS(IOError, "cannot open wal for read: " + path_);
    open_ = true;
    return STATUS(OK);
}

Status WalReader::ReadRecord(std::string* record) {
    if (!open_) return STATUS(IOError, "wal reader not open");
    // Inverse of AddRecord: pull 8-byte header, then exactly `length` payload
    // bytes, and verify masked CRC(payload) matches the stored checksum.
    char header[8];
    in_.read(header, 8);
    if (in_.eof() && in_.gcount() == 0) return STATUS(NotFound, "EOF");
    if (in_.gcount() != 8) return STATUS(Corruption, "truncated wal header");
    std::uint32_t len = DecodeFixed32(header);          // bytes 0..3
    std::uint32_t masked = DecodeFixed32(header + 4);   // bytes 4..7
    record->assign(len, '\0');
    if (len > 0) {
        in_.read(&(*record)[0], len);
        if (static_cast<std::uint32_t>(in_.gcount()) != len) {
            return STATUS(Corruption, "truncated wal record");
        }
    }
    std::uint32_t crc = Crc32(record->data(), record->size());
    if (MaskCrc(crc) != masked) return STATUS(Corruption, "wal crc mismatch");
    return STATUS(OK);
}

void WalReader::Close() {
    if (open_) {
        in_.close();
        open_ = false;
    }
}

std::string EncodeWriteBatch(std::uint64_t seq, const std::vector<BatchEntry>& entries) {
    // Logical payload (goes inside a framed WAL record):
    //   fixed64(start_seq) || fixed32(count) || entry*
    // entry = type_u8 || lenpref(user_key) || lenpref(value)
    // lenpref(x) = varint32(x.size()) || x bytes
    std::string out;
    PutFixed64(&out, seq);
    PutFixed32(&out, static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        out.push_back(static_cast<char>(e.first));           // ValueType
        PutLengthPrefixedSlice(&out, e.second.first);        // user key
        PutLengthPrefixedSlice(&out, e.second.second);       // value (empty on delete)
    }
    return out;
}

Status DecodeWriteBatch(const Slice& record, std::uint64_t* seq, std::vector<BatchEntry>* entries) {
    // Mirror of EncodeWriteBatch: walk the payload field-by-field. Each key and
    // value is recovered via GetLengthPrefixedSlice (varint length + bytes).
    Slice s = record;
    if (!GetFixed64(&s, seq)) return STATUS(Corruption, "batch missing seq");
    std::uint32_t count = 0;
    if (!GetFixed32(&s, &count)) return STATUS(Corruption, "batch missing count");
    entries->clear();
    entries->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (s.empty()) return STATUS(Corruption, "batch truncated type");
        auto type = static_cast<ValueType>(static_cast<unsigned char>(s[0]));
        s.remove_prefix(1);
        Slice key, val;
        if (!GetLengthPrefixedSlice(&s, &key)) return STATUS(Corruption, "batch bad key");
        if (!GetLengthPrefixedSlice(&s, &val)) return STATUS(Corruption, "batch bad val");
        entries->push_back({type, {key.ToString(), val.ToString()}});
    }
    return STATUS(OK);
}

}  // namespace lsmkv
