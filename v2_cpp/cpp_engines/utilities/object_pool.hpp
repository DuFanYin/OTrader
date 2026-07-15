#pragma once

/** Mempool + free list under one mutex. Explicit slot (storage + next). in_use set prevents
 * double-release. Shutdown waits for in-flight ops. ~T() must not throw. */

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace utilities {

template <typename T> class ObjectPool {
  public:
    struct Slot {
        alignas(T) unsigned char storage[sizeof(T)];
        Slot* next = nullptr;
    };
    static_assert(alignof(Slot) >= alignof(T), "Slot alignment");

    explicit ObjectPool(size_t chunk_size = 64) : chunk_size_(chunk_size) {
        if (chunk_size == 0) {
            throw std::invalid_argument("ObjectPool: chunk_size must be >= 1");
        }
    }

    ~ObjectPool() {
        close();
        {
            std::unique_lock lock(mutex_);
            shutdown_cv_.wait(lock,
                              [this] { return active_ops_.load(std::memory_order_acquire) == 0; });
            head_ = nullptr;
            free_count_ = 0;
            in_use_.clear();
            blocks_.clear();
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    void close() { closed_.store(true, std::memory_order_release); }
    bool is_closed() const { return closed_.load(std::memory_order_acquire); }

    T* acquire() { return emplace(); }
    template <typename... Args> T* emplace(Args&&... args) {
        if (closed_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        inc_active();
        struct Guard {
            ObjectPool* p;
            ~Guard() { p->dec_active(); }
        } guard{this};

        T* p = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (closed_.load(std::memory_order_acquire)) {
                return nullptr;
            }
            Slot* s = pop_slot_unlocked();
            if (s == nullptr) {
                grow_unlocked();
                s = pop_slot_unlocked();
            }
            if (s != nullptr) {
                p = ptr_from_slot(s);
                --free_count_;
                in_use_.insert(p);
            }
        }
        if (p == nullptr) {
            return nullptr;
        }
        try {
            new (p) T(std::forward<Args>(args)...);
        } catch (...) {
            std::lock_guard lock(mutex_);
            in_use_.erase(p);
            push_slot_unlocked(slot_from_ptr(p));
            ++free_count_;
            throw;
        }
        return p;
    }

    void release(T* p) noexcept {
        if (p == nullptr) {
            return;
        }
        std::lock_guard lock(mutex_);
#ifndef NDEBUG
        assert(is_owned_unlocked(p) && "release: pointer not from this pool");
        assert(in_use_.count(p) == 1 && "release: double-release or not acquired");
#endif
        if (in_use_.erase(p) == 0) {
            return;
        }
        p->~T();
        push_slot_unlocked(slot_from_ptr(p));
        ++free_count_;
    }

    bool release_checked(T* p) {
        if (p == nullptr) {
            return true;
        }
        std::lock_guard lock(mutex_);
#ifndef NDEBUG
        if (in_use_.count(p) != 1) {
            assert(false && "release_checked: double-release or not acquired");
        }
#endif
        if (!is_owned_unlocked(p) || in_use_.erase(p) == 0) {
            return false;
        }
        p->~T();
        push_slot_unlocked(slot_from_ptr(p));
        ++free_count_;
        return true;
    }

    size_t size() const {
        std::lock_guard lock(mutex_);
        return free_count_;
    }

  private:
    static T* ptr_from_slot(Slot* s) { return std::launder(reinterpret_cast<T*>(&s->storage)); }
    static Slot* slot_from_ptr(T* p) { return reinterpret_cast<Slot*>(p); }

    void inc_active() {
        std::lock_guard lock(mutex_);
        active_ops_.fetch_add(1, std::memory_order_relaxed);
    }

    void dec_active() {
        std::lock_guard lock(mutex_);
        if (active_ops_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            shutdown_cv_.notify_all();
        }
    }

    bool is_owned_unlocked(T* p) const {
        const uintptr_t p_addr = reinterpret_cast<uintptr_t>(p);
        for (const auto& block : blocks_) {
            const uintptr_t base_addr = reinterpret_cast<uintptr_t>(block.get());
            const size_t slot_size = sizeof(Slot);
            for (size_t i = 0; i < chunk_size_; ++i) {
                const uintptr_t slot_addr = base_addr + i * slot_size;
                if (p_addr == slot_addr) {
                    return true;
                }
            }
        }
        return false;
    }

    Slot* pop_slot_unlocked() {
        Slot* s = head_;
        if (s != nullptr) {
            head_ = s->next;
        }
        return s;
    }

    void push_slot_unlocked(Slot* s) {
        s->next = head_;
        head_ = s;
    }

    void grow_unlocked() {
        auto block = std::make_unique<Slot[]>(chunk_size_);
        Slot* base = block.get();
        for (size_t i = 0; i < chunk_size_; ++i) {
            push_slot_unlocked(base + i);
        }
        free_count_ += chunk_size_;
        blocks_.push_back(std::move(block));
    }

    size_t chunk_size_;
    std::atomic<bool> closed_{false};
    std::atomic<size_t> active_ops_{0};
    Slot* head_ = nullptr;
    size_t free_count_ = 0;
    std::unordered_set<T*> in_use_;
    mutable std::mutex mutex_;
    std::condition_variable_any shutdown_cv_;
    std::vector<std::unique_ptr<Slot[]>> blocks_;
};

} // namespace utilities
