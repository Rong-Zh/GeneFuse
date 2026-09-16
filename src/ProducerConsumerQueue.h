#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

/*
 * ProducerConsumerQueue is a one producer and one consumer queue
 * without locks.
 */
template <class T>
struct ProducerConsumerQueue
{
    typedef T value_type;

    ProducerConsumerQueue(const ProducerConsumerQueue &) = delete;
    ProducerConsumerQueue &operator=(const ProducerConsumerQueue &) = delete;

    // size must be >= 2.
    //
    // Also, note that the number of usable slots in the queue at any
    // given time is actually (size-1), so if you start with an empty queue,
    // isFull() will return true after size-1 insertions.
    explicit ProducerConsumerQueue(uint32_t size)
        : size_(size),
          records_(static_cast<T *>(std::malloc(sizeof(T) * size))),
          readIndex_(0),
          writeIndex_(0)
    {
        assert(size >= 2);
        if (!records_)
        {
            throw std::bad_alloc();
        }
    }

    ~ProducerConsumerQueue()
    {
        // We need to destruct anything that may still exist in our queue.
        // (No real synchronization needed at destructor time: only one
        // thread can be doing this.)
        if (!std::is_trivially_destructible<T>::value)
        {
            size_t readIndex = readIndex_;
            size_t endIndex = writeIndex_;
            while (readIndex != endIndex)
            {
                records_[readIndex].~T();
                if (++readIndex == size_)
                {
                    readIndex = 0;
                }
            }
        }

        std::free(records_);
    }

    template <class... Args>
    bool write(Args &&...recordArgs)
    {
        auto const currentWrite = writeIndex_.load(std::memory_order_relaxed);
        auto nextRecord = currentWrite + 1;
        if (nextRecord == size_)
        {
            nextRecord = 0;
        }
        if (nextRecord != readIndex_.load(std::memory_order_acquire))
        {
            new (&records_[currentWrite]) T(std::forward<Args>(recordArgs)...);
            writeIndex_.store(nextRecord, std::memory_order_release);
            writeIndex_.notify_one();
            return true;
        }

        // queue is full
        return false;
    }

    template <class... Args>
    void writeBlocking(Args &&...recordArgs)
    {
        auto currentWrite = writeIndex_.load(std::memory_order_relaxed);
        for (;;)
        {
            auto nextRecord = currentWrite + 1;
            if (nextRecord == size_)
                nextRecord = 0;

            auto observedRead = readIndex_.load(std::memory_order_acquire);
            if (nextRecord != observedRead)
            {
                new (&records_[currentWrite]) T(std::forward<Args>(recordArgs)...);
                writeIndex_.store(nextRecord, std::memory_order_release);
                writeIndex_.notify_one();
                return;
            }
            readIndex_.wait(observedRead, std::memory_order_relaxed);
            currentWrite = writeIndex_.load(std::memory_order_relaxed);
        }
    }

    // move (or copy) the value at the front of the queue to given variable
    bool read(T &record)
    {
        auto const currentRead = readIndex_.load(std::memory_order_relaxed);
        if (currentRead == writeIndex_.load(std::memory_order_acquire))
        {
            // queue is empty
            return false;
        }

        auto nextRecord = currentRead + 1;
        if (nextRecord == size_)
        {
            nextRecord = 0;
        }
        record = std::move(records_[currentRead]);
        records_[currentRead].~T();
        readIndex_.store(nextRecord, std::memory_order_release);
        readIndex_.notify_one();
        return true;
    }

    void readBlocking(T &record)
    {
        auto currentRead = readIndex_.load(std::memory_order_relaxed);
        for (;;)
        {
            auto observedWrite = writeIndex_.load(std::memory_order_acquire);
            if (currentRead != observedWrite)
            {
                auto nextRecord = currentRead + 1;
                if (nextRecord == size_)
                    nextRecord = 0;
                record = std::move(records_[currentRead]);
                records_[currentRead].~T();
                readIndex_.store(nextRecord, std::memory_order_release);
                readIndex_.notify_one();
                return;
            }
            writeIndex_.wait(observedWrite, std::memory_order_relaxed);
            currentRead = readIndex_.load(std::memory_order_relaxed);
        }
    }

    // pointer to the value at the front of the queue (for use in-place) or
    // nullptr if empty.
    T *frontPtr()
    {
        auto const currentRead = readIndex_.load(std::memory_order_relaxed);
        if (currentRead == writeIndex_.load(std::memory_order_acquire))
        {
            // queue is empty
            return nullptr;
        }
        return &records_[currentRead];
    }

    // queue must not be empty
    void popFront()
    {
        auto const currentRead = readIndex_.load(std::memory_order_relaxed);
        assert(currentRead != writeIndex_.load(std::memory_order_acquire));

        auto nextRecord = currentRead + 1;
        if (nextRecord == size_)
        {
            nextRecord = 0;
        }
        records_[currentRead].~T();
        readIndex_.store(nextRecord, std::memory_order_release);
        readIndex_.notify_one();
    }

    bool isEmpty() const
    {
        return readIndex_.load(std::memory_order_acquire) ==
               writeIndex_.load(std::memory_order_acquire);
    }

    bool isFull() const
    {
        auto nextRecord = writeIndex_.load(std::memory_order_acquire) + 1;
        if (nextRecord == size_)
        {
            nextRecord = 0;
        }
        if (nextRecord != readIndex_.load(std::memory_order_acquire))
        {
            return false;
        }
        // queue is full
        return true;
    }

    // * If called by consumer, then true size may be more (because producer may
    //   be adding items concurrently).
    // * If called by producer, then true size may be less (because consumer may
    //   be removing items concurrently).
    // * It is undefined to call this from any other thread.
    size_t sizeGuess() const
    {
        auto writeIndex = writeIndex_.load(std::memory_order_acquire);
        auto readIndex = readIndex_.load(std::memory_order_acquire);
        return writeIndex >= readIndex ? writeIndex - readIndex
                                       : size_ - (readIndex - writeIndex);
    }

    // maximum number of items in the queue.
    size_t capacity() const { return size_ - 1; }

private:
    static constexpr std::size_t kCacheLineSize = 128;
    using AtomicIndex = std::atomic<unsigned int>;

    char pad0_[kCacheLineSize];
    const uint32_t size_;
    T *const records_;

    alignas(kCacheLineSize) AtomicIndex readIndex_;
    alignas(kCacheLineSize) AtomicIndex writeIndex_;

    char pad1_[kCacheLineSize - sizeof(AtomicIndex)];
};
