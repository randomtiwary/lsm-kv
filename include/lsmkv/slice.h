#pragma once

#include <cassert>
#include <cstring>
#include <string>

namespace lsmkv {

// Non-owning view over a byte sequence. Caller keeps the backing storage alive.
class Slice {
public:
    Slice() : data_(""), size_(0) {}
    Slice(const char* d, std::size_t n) : data_(d), size_(n) {}
    Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
    Slice(const char* s) : data_(s), size_(s == nullptr ? 0 : std::strlen(s)) {}

    const char* data() const { return data_; }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    char operator[](std::size_t i) const {
        assert(i < size_);
        return data_[i];
    }

    void clear() {
        data_ = "";
        size_ = 0;
    }

    void remove_prefix(std::size_t n) {
        assert(n <= size_);
        data_ += n;
        size_ -= n;
    }

    std::string ToString() const { return std::string(data_, size_); }

    int compare(const Slice& b) const {
        const std::size_t min_len = (size_ < b.size_) ? size_ : b.size_;
        int r = min_len > 0 ? std::memcmp(data_, b.data_, min_len) : 0;
        if (r == 0) {
            if (size_ < b.size_) r = -1;
            else if (size_ > b.size_) r = 1;
        }
        return r;
    }

    bool starts_with(const Slice& x) const {
        return (size_ >= x.size_) && (std::memcmp(data_, x.data_, x.size_) == 0);
    }

private:
    const char* data_;
    std::size_t size_;
};

inline bool operator==(const Slice& a, const Slice& b) {
    return a.size() == b.size() && (a.size() == 0 || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
inline bool operator!=(const Slice& a, const Slice& b) { return !(a == b); }
inline bool operator<(const Slice& a, const Slice& b) { return a.compare(b) < 0; }

}  // namespace lsmkv
