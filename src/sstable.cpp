#include "lsmkv/sstable.h"

#include "lsmkv/encoding.h"
#include "lsmkv/crc32.h"
#include "lsmkv/internal_key.h"
#include "lsmkv/memtable.h"

namespace lsmkv {
namespace {
// Footer = fixed64 index_offset || fixed64 index_size || fixed32 magic.
const std::uint32_t kTableMagic = 0xDB04734Eu;
const std::size_t kFooterSize = 20;
}  // namespace

SSTableBuilder::SSTableBuilder(const Options& options, std::string path)
    : options_(options),
      path_(std::move(path)),
      data_block_(options.block_restart_interval),
      index_block_(1) {}

Status SSTableBuilder::Open() {
    out_.open(path_, std::ios::binary | std::ios::trunc);
    if (!out_) return STATUS(IOError, "cannot create sstable: " + path_);
    open_ = true;
    return STATUS(OK);
}

Status SSTableBuilder::WriteBlock(BlockBuilder* block, std::uint64_t* block_offset,
                                  std::uint64_t* block_size) {
    // On disk: block contents (see block.h) || fixed32(MaskCrc(crc32(contents))).
    // *block_size is contents only; CRC trailer is not part of the handle.
    Slice contents = block->Finish();
    std::string trailer;
    PutFixed32(&trailer, MaskCrc(Crc32(contents.data(), contents.size())));
    *block_offset = offset_;
    *block_size = contents.size();
    out_.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out_.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
    if (!out_) return STATUS(IOError, "sstable write block failed");
    offset_ += contents.size() + trailer.size();
    block->Reset();
    return STATUS(OK);
}

Status SSTableBuilder::FlushDataBlock() {
    if (data_block_.empty()) return STATUS(OK);
    std::uint64_t bo = 0, bs = 0;
    Status s = WriteBlock(&data_block_, &bo, &bs);
    if (!s.ok()) return s;
    pending_index_key_ = last_key_;
    pending_offset_ = bo;
    pending_size_ = bs;
    pending_index_entry_ = true;
    return STATUS(OK);
}

void SSTableBuilder::Add(const Slice& key, const Slice& value) {
    if (!status_.ok()) return;
    if (pending_index_entry_) {
        // Index value = BlockHandle: fixed64(offset) || fixed64(size).
        std::string handle;
        PutFixed64(&handle, pending_offset_);
        PutFixed64(&handle, pending_size_);
        index_block_.Add(pending_index_key_, handle);
        pending_index_entry_ = false;
    }
    if (!has_keys_) {
        smallest_ = key.ToString();
        has_keys_ = true;
    }
    largest_ = key.ToString();
    last_key_ = key.ToString();
    data_block_.Add(key, value);
    if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
        status_ = FlushDataBlock();
    }
}

Status SSTableBuilder::Finish(FileMetaData* meta) {
    if (!status_.ok()) return status_;
    Status s = FlushDataBlock();
    if (!s.ok()) return s;
    if (pending_index_entry_) {
        std::string handle;
        PutFixed64(&handle, pending_offset_);
        PutFixed64(&handle, pending_size_);
        index_block_.Add(pending_index_key_, handle);
        pending_index_entry_ = false;
    }
    std::uint64_t index_offset = 0, index_size = 0;
    s = WriteBlock(&index_block_, &index_offset, &index_size);
    if (!s.ok()) return s;
    // Footer points readers at the index block.
    std::string footer;
    PutFixed64(&footer, index_offset);
    PutFixed64(&footer, index_size);
    PutFixed32(&footer, kTableMagic);
    out_.write(footer.data(), static_cast<std::streamsize>(footer.size()));
    out_.flush();
    if (!out_) return STATUS(IOError, "sstable footer write failed");
    offset_ += footer.size();
    out_.close();
    open_ = false;
    if (meta != nullptr) {
        meta->file_size = offset_;
        meta->smallest = smallest_;
        meta->largest = largest_;
    }
    return STATUS(OK);
}

SSTable::SSTable(std::string path) : path_(std::move(path)) {}

Status SSTable::ReadBlock(std::uint64_t offset, std::uint64_t size, std::string* result) const {
    // Inverse of WriteBlock: slice contents, verify masked CRC in the 4 bytes after.
    if (offset + size + 4 > file_data_.size()) return STATUS(Corruption, "block out of range");
    Slice contents(file_data_.data() + offset, static_cast<std::size_t>(size));
    std::uint32_t masked = DecodeFixed32(file_data_.data() + offset + size);
    if (MaskCrc(Crc32(contents.data(), contents.size())) != masked) {
        return STATUS(Corruption, "block crc mismatch");
    }
    result->assign(contents.data(), contents.size());
    return STATUS(OK);
}

Status SSTable::Open(const std::string& path, std::unique_ptr<SSTable>* table) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return STATUS(IOError, "cannot open sstable: " + path);
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    if (sz < static_cast<std::streamoff>(kFooterSize)) return STATUS(Corruption, "sstable too small");
    in.seekg(0);
    auto t = std::unique_ptr<SSTable>(new SSTable(path));
    t->file_data_.assign(static_cast<std::size_t>(sz), '\0');
    in.read(&t->file_data_[0], sz);
    if (!in) return STATUS(IOError, "read sstable failed");
    // Parse footer from the last 20 bytes.
    const char* footer = t->file_data_.data() + t->file_data_.size() - kFooterSize;
    t->index_offset_ = DecodeFixed64(footer);
    t->index_size_ = DecodeFixed64(footer + 8);
    std::uint32_t magic = DecodeFixed32(footer + 16);
    if (magic != kTableMagic) return STATUS(Corruption, "bad sstable magic");
    *table = std::move(t);
    return STATUS(OK);
}

Status SSTable::Get(const Slice& internal_key, std::string* value, bool* found) const {
    // index seek -> BlockHandle -> data block seek -> user-key / seq check.
    *found = false;
    std::string index_data;
    Status s = ReadBlock(index_offset_, index_size_, &index_data);
    if (!s.ok()) return s;
    Block index(std::move(index_data));
    if (!index.status().ok()) return index.status();
    auto it = index.NewIterator();
    it.Seek(internal_key);
    if (!it.Valid()) return STATUS(OK);
    Slice handle = it.value();
    if (handle.size() < 16) return STATUS(Corruption, "bad handle");
    std::uint64_t bo = DecodeFixed64(handle.data());
    std::uint64_t bs = DecodeFixed64(handle.data() + 8);
    std::string block_data;
    s = ReadBlock(bo, bs, &block_data);
    if (!s.ok()) return s;
    Block data(std::move(block_data));
    auto dit = data.NewIterator();
    dit.Seek(internal_key);
    if (!dit.Valid()) return STATUS(OK);
    Slice user = ExtractUserKey(internal_key);
    if (ExtractUserKey(dit.key()).compare(user) != 0) return STATUS(OK);
    std::uint64_t snapshot = ExtractSequence(internal_key);
    if (ExtractSequence(dit.key()) > snapshot) return STATUS(OK);
    if (ExtractValueType(dit.key()) == kTypeDeletion) {
        *found = true;
        value->clear();
        return STATUS(NotFound);
    }
    value->assign(dit.value().data(), dit.value().size());
    *found = true;
    return STATUS(OK);
}

std::unique_ptr<SSTable::Iterator> SSTable::NewIterator() const {
    return std::unique_ptr<Iterator>(new Iterator(this));
}

SSTable::Iterator::Iterator(const SSTable* table) : table_(table) {
    std::string index_data;
    status_ = table_->ReadBlock(table_->index_offset_, table_->index_size_, &index_data);
    if (!status_.ok()) return;
    index_block_.reset(new Block(std::move(index_data)));
    status_ = index_block_->status();
    if (!status_.ok()) return;
    index_iter_.reset(new Block::Iterator(index_block_->NewIterator()));
}

SSTable::Iterator::~Iterator() = default;

void SSTable::Iterator::InitDataBlock() {
    data_iter_.reset();
    data_block_.reset();
    if (!index_iter_ || !index_iter_->Valid()) return;
    Slice handle = index_iter_->value();
    if (handle.size() < 16) {
        status_ = STATUS(Corruption, "bad handle");
        return;
    }
    std::uint64_t bo = DecodeFixed64(handle.data());
    std::uint64_t bs = DecodeFixed64(handle.data() + 8);
    std::string block_data;
    status_ = table_->ReadBlock(bo, bs, &block_data);
    if (!status_.ok()) return;
    data_block_.reset(new Block(std::move(block_data)));
    status_ = data_block_->status();
    if (!status_.ok()) return;
    data_iter_.reset(new Block::Iterator(data_block_->NewIterator()));
}

bool SSTable::Iterator::Valid() const {
    return data_iter_ && data_iter_->Valid();
}

Slice SSTable::Iterator::key() const { return data_iter_->key(); }
Slice SSTable::Iterator::value() const { return data_iter_->value(); }

void SSTable::Iterator::Next() {
    data_iter_->Next();
    if (!data_iter_->Valid()) {
        index_iter_->Next();
        if (index_iter_->Valid()) {
            InitDataBlock();
            if (data_iter_) data_iter_->SeekToFirst();
        }
    }
}

void SSTable::Iterator::SeekToFirst() {
    if (!index_iter_) return;
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_) data_iter_->SeekToFirst();
}

void SSTable::Iterator::Seek(const Slice& target) {
    if (!index_iter_) return;
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_) data_iter_->Seek(target);
}

Status BuildTableFromMemTable(const Options& options, const std::string& path,
                              const MemTable& mem, std::uint64_t file_number,
                              FileMetaData* meta) {
    SSTableBuilder builder(options, path);
    Status s = builder.Open();
    if (!s.ok()) return s;
    mem.ForEach([&](const Slice& ikey, const Slice& val) { builder.Add(ikey, val); });
    s = builder.Finish(meta);
    if (!s.ok()) return s;
    if (meta) meta->number = file_number;
    return STATUS(OK);
}

}  // namespace lsmkv
