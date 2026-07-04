#pragma once

#include <cassert>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace lsmkv {

// Simple educational skip list.
// - Lookups and iterators take a shared lock; inserts take an exclusive lock.
// - An Iterator holds that shared lock for its entire lifetime so walking
//   node links stays race-free with concurrent writers.
// Comparator must provide: int operator()(const Key& a, const Key& b) const
// returning <0, 0, >0 like memcmp.
template <typename Key, typename Comparator>
class SkipList {
public:
    static const int kMaxHeight = 12;

    explicit SkipList(Comparator cmp = Comparator())
        : compare_(cmp), head_(NewNode(Key(), kMaxHeight)), max_height_(1), size_(0) {
        for (int i = 0; i < kMaxHeight; ++i) head_->next[i] = nullptr;
    }

    ~SkipList() {
        Node* n = head_;
        while (n != nullptr) {
            Node* next = n->next[0];
            n->~Node();
            ::operator delete(n);
            n = next;
        }
    }

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    void Insert(const Key& key) {
        std::unique_lock<std::shared_mutex> lock(mu_);
        Node* prev[kMaxHeight];
        Node* x = FindGreaterOrEqual(key, prev);
        if (x != nullptr && Equal(key, x->key)) {
            x->key = key;  // update in place for equal keys
            return;
        }
        int height = RandomHeight();
        if (height > max_height_) {
            for (int i = max_height_; i < height; ++i) prev[i] = head_;
            max_height_ = height;
        }
        x = NewNode(key, height);
        for (int i = 0; i < height; ++i) {
            x->next[i] = prev[i]->next[i];
            prev[i]->next[i] = x;
        }
        ++size_;
    }

    bool Contains(const Key& key) const {
        std::shared_lock<std::shared_mutex> lock(mu_);
        Node* x = FindGreaterOrEqual(key, nullptr);
        return x != nullptr && Equal(key, x->key);
    }

    // Find the first entry >= key. Returns false if none.
    bool Find(const Key& key, Key* out) const {
        std::shared_lock<std::shared_mutex> lock(mu_);
        Node* x = FindGreaterOrEqual(key, nullptr);
        if (x == nullptr) return false;
        *out = x->key;
        return true;
    }

    std::size_t Size() const {
        std::shared_lock<std::shared_mutex> lock(mu_);
        return size_;
    }

private:
    struct Node {
        Key key;
        Node* next[1];  // actual size = height
    };

public:
    // Holds a shared lock on the skip list for its lifetime. Multiple iterators
    // (and Contains/Find) may run together; Insert blocks until they are destroyed.
    class Iterator {
    public:
        explicit Iterator(const SkipList* list)
            : list_(list), lock_(list->mu_), node_(nullptr) {}

        Iterator(const Iterator&) = delete;
        Iterator& operator=(const Iterator&) = delete;

        Iterator(Iterator&& other) noexcept
            : list_(other.list_), lock_(std::move(other.lock_)), node_(other.node_) {
            other.list_ = nullptr;
            other.node_ = nullptr;
        }

        Iterator& operator=(Iterator&&) = delete;

        bool Valid() const { return node_ != nullptr; }

        const Key& key() const {
            assert(Valid());
            return node_->key;
        }

        void Next() {
            assert(Valid());
            node_ = node_->next[0];
        }

        void SeekToFirst() {
            assert(list_ != nullptr);
            node_ = list_->head_->next[0];
        }

        void Seek(const Key& target) {
            assert(list_ != nullptr);
            node_ = list_->FindGreaterOrEqual(target, nullptr);
        }

    private:
        const SkipList* list_;
        std::shared_lock<std::shared_mutex> lock_;
        Node* node_;
    };

    Iterator NewIterator() const { return Iterator(this); }

    template <typename Fn>
    void Iterate(Fn fn) const {
        Iterator it = NewIterator();
        for (it.SeekToFirst(); it.Valid(); it.Next()) {
            fn(it.key());
        }
    }

private:
    Comparator compare_;
    Node* head_;
    int max_height_;
    std::size_t size_;
    mutable std::shared_mutex mu_;
    mutable unsigned int rnd_ = 0xdeadbeef;

    Node* NewNode(const Key& key, int height) {
        std::size_t bytes = sizeof(Node) + sizeof(Node*) * static_cast<std::size_t>(height - 1);
        void* mem = ::operator new(bytes);
        Node* n = new (mem) Node();
        n->key = key;
        for (int i = 0; i < height; ++i) n->next[i] = nullptr;
        return n;
    }

    int RandomHeight() {
        static const unsigned int kBranching = 4;
        int height = 1;
        while (height < kMaxHeight && (Random() % kBranching) == 0) ++height;
        return height;
    }

    unsigned int Random() {
        rnd_ = rnd_ * 214013u + 2531011u;
        return rnd_ >> 16;
    }

    bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }

    // Caller must hold mu_ (shared or exclusive).
    Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
        Node* x = head_;
        int level = max_height_ - 1;
        while (true) {
            Node* next = x->next[level];
            if (next != nullptr && compare_(next->key, key) < 0) {
                x = next;
            } else {
                if (prev != nullptr) prev[level] = x;
                if (level == 0) return next;
                --level;
            }
        }
    }
};

}  // namespace lsmkv
