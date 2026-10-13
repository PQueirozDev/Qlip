#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

// Fila limitada entre threads. push() bloqueia quando cheia (backpressure);
// tryPush() não bloqueia e permite descartar o item quando a fila está cheia.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

    bool push(T item) {
        std::unique_lock lock(mutex_);
        notFull_.wait(lock, [this] { return items_.size() < capacity_ || closed_; });
        if (closed_) return false;
        items_.push_back(std::move(item));
        notEmpty_.notify_one();
        return true;
    }

    bool tryPush(T item) {
        std::lock_guard lock(mutex_);
        if (closed_ || items_.size() >= capacity_) return false;
        items_.push_back(std::move(item));
        notEmpty_.notify_one();
        return true;
    }

    // Retorna false quando a fila foi fechada e não há mais itens.
    bool pop(T& out) {
        std::unique_lock lock(mutex_);
        notEmpty_.wait(lock, [this] { return !items_.empty() || closed_; });
        if (items_.empty()) return false;
        out = std::move(items_.front());
        items_.pop_front();
        notFull_.notify_one();
        return true;
    }

    enum class PopResult { Item, Timeout, Closed };

    // Espera até timeout por um item. Closed = fila fechada e vazia.
    PopResult popFor(T& out, std::chrono::microseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!notEmpty_.wait_for(lock, timeout, [this] { return !items_.empty() || closed_; })) return PopResult::Timeout;
        if (items_.empty()) return PopResult::Closed;
        out = std::move(items_.front());
        items_.pop_front();
        notFull_.notify_one();
        return PopResult::Item;
    }

    bool isClosed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<T> items_;
    size_t capacity_;
    bool closed_ = false;
};
