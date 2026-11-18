#pragma once

#include "checksum.hpp"
#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

// The caller remains the sole owner of the device and its borrowed RX buffer.
// Workers finish reading the span before calculate() returns.
class ChecksumWorkers {
  public:
    explicit ChecksumWorkers(unsigned count) : results_(count), count_(count) {
        try {
            for (unsigned i = 1; i < count_; ++i) {
                threads_.emplace_back([this, i] { work(i); });
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~ChecksumWorkers() {
        shutdown();
    }

    ChecksumWorkers(const ChecksumWorkers &) = delete;
    ChecksumWorkers &operator=(const ChecksumWorkers &) = delete;

    Checksum calculate(std::span<const acceldoku::PuzzleRecord> records) {
        if (count_ == 1) {
            return chunk(records, 0);
        }
        {
            std::lock_guard lock(mutex_);
            records_ = records;
            remaining_ = count_ - 1;
            ++generation_;
        }
        start_.notify_all();
        auto result = chunk(records, 0);
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return remaining_ == 0; });
        for (unsigned i = 1; i < count_; ++i) {
            result.merge(results_[i].value);
        }
        return result;
    }

  private:
    struct alignas(64) Result {
        Checksum value;
    };

    Checksum chunk(std::span<const acceldoku::PuzzleRecord> records, unsigned i) const {
        const auto begin = records.size() * i / count_;
        const auto end = records.size() * (i + 1) / count_;
        Checksum result;
        for (const auto &record : records.subspan(begin, end - begin)) {
            result.add(record);
        }
        return result;
    }

    void work(unsigned i) {
        std::unique_lock lock(mutex_);
        std::size_t seen = 0;
        for (;;) {
            start_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_) {
                return;
            }
            seen = generation_;
            auto records = records_;
            lock.unlock();
            auto result = chunk(records, i);
            lock.lock();
            results_[i].value = result;
            if (--remaining_ == 0) {
                done_.notify_one();
            }
        }
    }

    void shutdown() noexcept {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        start_.notify_all();
        for (auto &thread : threads_) {
            thread.join();
        }
    }

    std::vector<Result> results_;
    unsigned count_;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable start_, done_;
    std::span<const acceldoku::PuzzleRecord> records_;
    std::size_t generation_ = 0;
    unsigned remaining_ = 0;
    bool stop_ = false;
};
