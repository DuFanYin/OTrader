#pragma once

// Bounded ring buffer with no synchronisation, from Option-Pricing
// (Queue/RingBuffer.hpp). The original test shared it between two threads
// without a lock, which is a data race; here it is the baseline that the
// benchmark wraps in a mutex (LockedQueue below).

#include <cassert>
#include <memory>
#include <mutex>
#include <stdexcept>

template <class T>
class RingBuffer {
public:
    explicit RingBuffer(unsigned size) : size_(size), data_(std::make_unique<T[]>(size)) { assert(size > 1); }

    bool empty() const { return front_ == rear_; }
    bool full()  const { return (rear_ + 1) % size_ == front_; }

    bool push(const T& v) {
        if (full()) return false;
        data_[rear_] = v;
        rear_ = (rear_ + 1) % size_;
        return true;
    }

    bool pop(T& v) {
        if (empty()) return false;
        v = data_[front_];
        front_ = (front_ + 1) % size_;
        return true;
    }

private:
    unsigned size_;
    unsigned front_ = 0, rear_ = 0;
    std::unique_ptr<T[]> data_;
};

// RingBuffer behind one mutex: safe for any number of producers / consumers.
template <class T>
class LockedQueue {
public:
    explicit LockedQueue(unsigned size) : rb_(size) {}
    bool push(const T& v) { std::lock_guard lk(m_); return rb_.push(v); }
    bool pop(T& v)        { std::lock_guard lk(m_); return rb_.pop(v); }
private:
    std::mutex m_;
    RingBuffer<T> rb_;
};
