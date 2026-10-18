#pragma once

/**
 * MPSC (multi producer, single consumer) queue, unbounded and intrusive (D. Vyukov's): the caller's
 * nodes are linked through their own `std::atomic<Node*> next`, so a push allocates nothing and
 * never waits (one atomic exchange). Nodes come out in the order they were pushed. Node must be
 * default-constructible (the queue keeps one as its stub).
 *
 * A push between its exchange and its link leaves the queue cut there for a moment: pop() returns
 * null while empty() is false; the consumer tries again (the producer is a few instructions from
 * done).
 */

#include <atomic>

namespace utilities {

template <typename Node> class MpscQueue {
  public:
    MpscQueue() : head_(&stub_), tail_(&stub_) {}
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    void push(Node* node) {
        node->next.store(nullptr, std::memory_order_relaxed);
        Node* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    /** The consumer: the oldest node, or null (empty, or a push not linked yet). */
    Node* pop() {
        Node* tail = tail_;
        Node* next = tail->next.load(std::memory_order_acquire);
        if (tail == &stub_) {
            if (next == nullptr) {
                return nullptr;
            }
            tail_ = next;
            tail = next;
            next = next->next.load(std::memory_order_acquire);
        }
        if (next != nullptr) {
            tail_ = next;
            return tail;
        }
        if (tail != head_.load(std::memory_order_acquire)) {
            return nullptr; // a push under way behind tail
        }
        push(&stub_); // tail is the last node: the stub goes behind it, so it can come out
        next = tail->next.load(std::memory_order_acquire);
        if (next != nullptr) {
            tail_ = next;
            return tail;
        }
        return nullptr;
    }

    /** The consumer: nothing pushed that has not come out (a push under way counts as there). */
    [[nodiscard]] bool empty() const {
        return tail_ == &stub_ && head_.load(std::memory_order_acquire) == &stub_;
    }

  private:
    alignas(64) std::atomic<Node*> head_; // the newest: producers exchange it
    alignas(64) Node* tail_;              // the oldest not out yet, or the stub: the consumer's
    Node stub_;
};

} // namespace utilities
