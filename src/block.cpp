#include "lsmkv/block.h"
#include <cassert>

#include <algorithm>

#include "lsmkv/encoding.h"
#include "lsmkv/internal_key.h"

namespace lsmkv {

BlockBuilder::BlockBuilder(int restart_interval) : restart_interval_(restart_interval) {
    restarts_.push_back(0);
}

void BlockBuilder::Reset() {
    buffer_.clear();
    restarts_.clear();
    restarts_.push_back(0);
    last_key_.clear();
    counter_ = 0;
    finished_ = false;
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
    assert(!finished_);
    assert(counter_ <= restart_interval_);
    // Prefix-compress against last_key_ unless this entry starts a restart
    // interval (shared forced to 0, offset recorded in restarts_).
    std::size_t shared = 0;
    if (counter_ < restart_interval_) {
        const std::size_t min_len = std::min(last_key_.size(), key.size());
        while (shared < min_len && last_key_[shared] == key[shared]) ++shared;
    } else {
        restarts_.push_back(static_cast<std::uint32_t>(buffer_.size()));
        counter_ = 0;
    }
    const std::size_t non_shared = key.size() - shared;
    // entry = varint(shared) | varint(non_shared) | varint(vlen) | key_delta | value
    PutVarint32(&buffer_, static_cast<std::uint32_t>(shared));
    PutVarint32(&buffer_, static_cast<std::uint32_t>(non_shared));
    PutVarint32(&buffer_, static_cast<std::uint32_t>(value.size()));
    buffer_.append(key.data() + shared, non_shared);
    buffer_.append(value.data(), value.size());
    last_key_.resize(shared);
    last_key_.append(key.data() + shared, non_shared);
    ++counter_;
}

Slice BlockBuilder::Finish() {
    // Trailer: fixed32 restart_offset[i]... || fixed32 num_restarts.
    for (std::uint32_t r : restarts_) PutFixed32(&buffer_, r);
    PutFixed32(&buffer_, static_cast<std::uint32_t>(restarts_.size()));
    finished_ = true;
    return Slice(buffer_);
}

std::size_t BlockBuilder::CurrentSizeEstimate() const {
    return buffer_.size() + restarts_.size() * 4 + 4;
}

Block::Block(std::string data) : data_(std::move(data)) {
    if (data_.size() < 4) {
        status_ = STATUS(Corruption, "block too small");
        return;
    }
    // Last fixed32 is num_restarts; restart array sits just before it.
    restart_offset_ = static_cast<std::uint32_t>(data_.size()) - 4 -
                      DecodeFixed32(data_.data() + data_.size() - 4) * 4;
    if (restart_offset_ > data_.size() - 4) {
        status_ = STATUS(Corruption, "bad restart offset");
    }
}

Block::Iterator::Iterator(const Block* block) : block_(block), status_(block->status()) {}

std::uint32_t Block::Iterator::NumRestarts() const {
    return DecodeFixed32(block_->data_.data() + block_->data_.size() - 4);
}

const char* Block::Iterator::RestartPoint(std::uint32_t index) const {
    return block_->data_.data() + DecodeFixed32(block_->data_.data() + block_->restart_offset_ + index * 4);
}

void Block::Iterator::ParseNext() {
    // Decode one entry and rebuild the full key from key_buf_ prefix + delta.
    if (p_ >= limit_) {
        valid_ = false;
        return;
    }
    std::uint32_t shared = 0, non_shared = 0, value_len = 0;
    const char* q = GetVarint32Ptr(p_, limit_, &shared);
    if (q) q = GetVarint32Ptr(q, limit_, &non_shared);
    if (q) q = GetVarint32Ptr(q, limit_, &value_len);
    if (!q || static_cast<std::size_t>(limit_ - q) < non_shared + value_len || shared > key_buf_.size()) {
        valid_ = false;
        status_ = STATUS(Corruption, "bad block entry");
        return;
    }
    key_buf_.resize(shared);
    key_buf_.append(q, non_shared);
    key_ = Slice(key_buf_);
    value_ = Slice(q + non_shared, value_len);
    p_ = q + non_shared + value_len;
    valid_ = true;
}

void Block::Iterator::Next() {
    assert(valid_);
    ParseNext();
}

void Block::Iterator::SeekToFirst() {
    if (!status_.ok()) { valid_ = false; return; }
    restart_index_ = 0;
    p_ = RestartPoint(0);
    limit_ = block_->data_.data() + block_->restart_offset_;
    key_buf_.clear();
    ParseNext();
}

void Block::Iterator::Seek(const Slice& target) {
    if (!status_.ok()) { valid_ = false; return; }
    // Binary search restart keys (always fully stored), then linear scan.
    std::uint32_t left = 0;
    std::uint32_t right = NumRestarts();
    while (left + 1 < right) {
        std::uint32_t mid = (left + right) / 2;
        const char* region = RestartPoint(mid);
        std::uint32_t shared = 0, non_shared = 0, value_len = 0;
        const char* q = GetVarint32Ptr(region, block_->data_.data() + block_->restart_offset_, &shared);
        q = GetVarint32Ptr(q, block_->data_.data() + block_->restart_offset_, &non_shared);
        q = GetVarint32Ptr(q, block_->data_.data() + block_->restart_offset_, &value_len);
        Slice mid_key(q, non_shared);  // shared must be 0 at restart
        if (CompareInternalKey(mid_key, target) < 0) left = mid;
        else right = mid;
    }
    restart_index_ = left;
    p_ = RestartPoint(left);
    limit_ = block_->data_.data() + block_->restart_offset_;
    key_buf_.clear();
    ParseNext();
    while (valid_ && CompareInternalKey(key_, target) < 0) ParseNext();
}

}  // namespace lsmkv
