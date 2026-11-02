#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace sa {

class FrameRing {
public:
    explicit FrameRing(size_t frames) : cap_(frames), buf_(frames * 2) {}

    size_t available() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }

    size_t push(const float* src, size_t frames) {
        const size_t w = w_.load(std::memory_order_relaxed);
        const size_t free = cap_ - (w - r_.load(std::memory_order_acquire));
        const size_t n = frames < free ? frames : free;
        for (size_t i = 0; i < n; ++i) {
            const size_t slot = (w + i) % cap_;
            buf_[2 * slot] = src ? src[2 * i] : 0.0f;
            buf_[2 * slot + 1] = src ? src[2 * i + 1] : 0.0f;
        }
        w_.store(w + n, std::memory_order_release);
        return n;
    }

    size_t pop(float* dst, size_t frames) {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t have = w_.load(std::memory_order_acquire) - r;
        const size_t n = frames < have ? frames : have;
        for (size_t i = 0; i < n; ++i) {
            const size_t slot = (r + i) % cap_;
            dst[2 * i] = buf_[2 * slot];
            dst[2 * i + 1] = buf_[2 * slot + 1];
        }
        r_.store(r + n, std::memory_order_release);
        return n;
    }

    // Reader side only: drops the oldest frames so that at most `keep` remain.
    void trim(size_t keep) {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t have = w_.load(std::memory_order_acquire) - r;
        if (have > keep) r_.store(r + (have - keep), std::memory_order_release);
    }

    void clear() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    size_t cap_;
    std::vector<float> buf_;
    std::atomic<size_t> r_{0}, w_{0};
};

}
