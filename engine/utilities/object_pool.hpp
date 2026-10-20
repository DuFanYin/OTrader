#pragma once

/** A pool of T objects, lock-free: a free list (a Treiber stack of slot indices, tagged against
 * ABA) over chunks that are allocated as it grows and kept until the pool goes. Each object is
 * constructed once, with its chunk, and destroyed with the pool: acquire() hands one out set to
 * T{}, take() as it was released, its buffers kept (strings' and vectors' alike), so a payload
 * copied in whole allocates nothing once warm. A second release of the same object is ignored.
 * Destroy the pool once no thread uses it. */

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace utilities {

template <typename T> class ObjectPool {
  public:
    /** chunk_size: the first chunk's objects (rounded up to a power of two); each next chunk is
     * twice the one before. */
    explicit ObjectPool(size_t chunk_size = 64) {
        if (chunk_size == 0) {
            throw std::invalid_argument("ObjectPool: chunk_size must be >= 1");
        }
        first_bits_ = static_cast<unsigned>(std::bit_width(std::bit_ceil(chunk_size)) - 1);
        max_chunks_ = first_bits_ < 31 - kMaxChunks ? kMaxChunks : 31 - first_bits_;
    }

    ~ObjectPool() {
        for (size_t c = 0; c < kMaxChunks; ++c) {
            Slot* chunk = chunks_[c].load(std::memory_order_acquire);
            if (chunk == nullptr) {
                continue;
            }
            const size_t n = chunk_objects(c);
            for (size_t i = 0; i < n; ++i) {
                object_of(&chunk[i])->~T();
            }
            delete[] chunk;
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    void close() { closed_.store(true, std::memory_order_release); }

    /** An object set to T{}; null once closed. */
    T* acquire() { return emplace(); }
    /** An object set to T(args...); null once closed. */
    template <typename... Args> T* emplace(Args&&... args) {
        Slot* s = free_slot();
        if (s == nullptr) {
            return nullptr;
        }
        try {
            *object_of(s) = T(std::forward<Args>(args)...);
        } catch (...) {
            push(s);
            throw;
        }
        return hand_out(s);
    }
    /** An object as it was released, with its values and buffers, for one assigned whole; null
     * once closed. */
    T* take() {
        Slot* s = free_slot();
        return s != nullptr ? hand_out(s) : nullptr;
    }

    void release(T* p) noexcept {
        if (p == nullptr) {
            return;
        }
        Slot* s = slot_of(p);
        assert(s != nullptr && "release: pointer not from this pool");
        release_slot(s); // released already: ignored
    }

    /** False when p is not one of this pool's objects handed out. */
    bool release_checked(T* p) { return p == nullptr || release_slot(slot_of(p)); }

    /** The objects free now (a count for tests; approximate while other threads use the pool). */
    size_t size() const {
        size_t n = 0;
        const size_t limit = objects_.load(std::memory_order_acquire);
        for (std::uint32_t i = index_of(head_.load(std::memory_order_acquire)); i != kNone && n < limit;
             i = slot(i)->next.load(std::memory_order_relaxed)) {
            ++n;
        }
        return n;
    }

  private:
    /** An object's storage first (a slot and its object share an address), then its links. */
    struct Slot {
        alignas(T) unsigned char storage[sizeof(T)];
        std::uint32_t index = kNone;            // its own, set before it is first free
        std::atomic<std::uint32_t> next{kNone}; // the next free slot's index
        std::atomic<bool> out{false};           // handed out, not released yet
    };

    static constexpr std::uint32_t kNone = UINT32_MAX;
    static constexpr size_t kMaxChunks = 24; // fewer for a large first chunk: indices fit 31 bits

    static T* object_of(Slot* s) { return std::launder(reinterpret_cast<T*>(&s->storage)); }

    // The head: a slot index (low 32 bits) and a tag counting the changes (high 32 bits).
    static std::uint32_t index_of(std::uint64_t head) { return static_cast<std::uint32_t>(head); }
    static std::uint64_t head_of(std::uint32_t index, std::uint64_t old) {
        return ((old >> 32) + 1) << 32 | index;
    }

    /** Chunk c holds chunk_size << c objects; its first index is chunk_size * (2^c - 1). */
    size_t chunk_objects(size_t c) const { return size_t{1} << (first_bits_ + c); }
    Slot* slot(std::uint32_t index) const {
        const std::uint64_t q = std::uint64_t{index} + (std::uint64_t{1} << first_bits_);
        const auto c = static_cast<size_t>(std::bit_width(q) - 1 - first_bits_);
        const std::uint64_t first = (std::uint64_t{1} << (first_bits_ + c)) - (std::uint64_t{1} << first_bits_);
        return chunks_[c].load(std::memory_order_acquire) + (index - first);
    }

    /** The slot whose object p is, found by address among the chunks; null for any other
     * pointer. */
    Slot* slot_of(const T* p) const {
        const auto at = reinterpret_cast<std::uintptr_t>(p);
        const size_t chunks = std::min(chunks_used_.load(std::memory_order_acquire), max_chunks_);
        for (size_t c = 0; c < chunks; ++c) {
            Slot* chunk = chunks_[c].load(std::memory_order_acquire);
            if (chunk == nullptr) {
                continue;
            }
            const auto first = reinterpret_cast<std::uintptr_t>(chunk);
            if (at >= first && at < first + chunk_objects(c) * sizeof(Slot)) {
                const size_t offset = at - first;
                return offset % sizeof(Slot) == 0 ? chunk + offset / sizeof(Slot) : nullptr;
            }
        }
        return nullptr;
    }

    /** A slot off the free list, or a new chunk's; null once closed or every chunk is used. */
    Slot* free_slot() {
        if (closed_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        Slot* s = pop();
        return s != nullptr ? s : grow();
    }

    T* hand_out(Slot* s) {
        s->out.store(true, std::memory_order_relaxed);
        return object_of(s);
    }

    Slot* pop() {
        std::uint64_t head = head_.load(std::memory_order_acquire);
        for (;;) {
            const std::uint32_t index = index_of(head);
            if (index == kNone) {
                return nullptr;
            }
            Slot* s = slot(index);
            // Stale when another thread took s meanwhile: the tag has changed, the CAS fails.
            const std::uint32_t next = s->next.load(std::memory_order_relaxed);
            if (head_.compare_exchange_weak(head, head_of(next, head), std::memory_order_acquire,
                                            std::memory_order_acquire)) {
                return s;
            }
        }
    }

    void push(Slot* s) { push_chain(s, s); }

    /** Onto the free list if handed out; false for null or one released already. */
    bool release_slot(Slot* s) noexcept {
        if (s == nullptr || !s->out.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }
        push(s);
        return true;
    }

    /** Push the chain first..last (linked through next) in one go. */
    void push_chain(Slot* first, Slot* last) {
        std::uint64_t head = head_.load(std::memory_order_relaxed);
        do {
            last->next.store(index_of(head), std::memory_order_relaxed);
        } while (!head_.compare_exchange_weak(head, head_of(first->index, head),
                                              std::memory_order_release, std::memory_order_relaxed));
    }

    /** A new chunk (each thread that finds the list empty adds its own): its objects constructed,
     * all but one onto the free list; that one is returned. Null once every chunk is used. */
    Slot* grow() {
        const size_t c = chunks_used_.fetch_add(1, std::memory_order_relaxed);
        if (c >= max_chunks_) {
            return nullptr;
        }
        const size_t n = chunk_objects(c);
        Slot* chunk = new Slot[n];
        const auto first = static_cast<std::uint32_t>(n - chunk_objects(0));
        for (size_t i = 0; i < n; ++i) {
            new (&chunk[i].storage) T();
            chunk[i].index = first + static_cast<std::uint32_t>(i);
            if (i + 1 < n) {
                chunk[i].next.store(chunk[i].index + 1, std::memory_order_relaxed);
            }
        }
        chunks_[c].store(chunk, std::memory_order_release);
        objects_.fetch_add(n, std::memory_order_relaxed);
        if (n > 1) {
            push_chain(&chunk[1], &chunk[n - 1]);
        }
        return &chunk[0];
    }

    unsigned first_bits_ = 6;
    size_t max_chunks_ = kMaxChunks;
    alignas(64) std::atomic<std::uint64_t> head_{kNone};
    alignas(64) std::atomic<bool> closed_{false};
    std::atomic<size_t> chunks_used_{0};
    std::atomic<size_t> objects_{0};
    std::array<std::atomic<Slot*>, kMaxChunks> chunks_{};
};

} // namespace utilities
