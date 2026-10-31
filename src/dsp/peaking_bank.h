#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "biquad.h"

namespace sa {

template <size_t N>
class PeakingBank {
public:
    static constexpr size_t kBlock = 32;

    void configure(const std::array<double, N>& freqs, double q, double rate) {
        freqs_ = freqs;
        q_ = q;
        rate_ = rate;
        for (size_t b = 0; b < N; ++b) refresh(b);
    }

    void setTarget(const std::array<float, N>& db, float maxStepDb, float approach) {
        target_ = db;
        maxStep_ = maxStepDb;
        approach_ = approach;
    }

    const std::array<float, N>& current() const { return current_; }

    bool idle() const {
        for (size_t b = 0; b < N; ++b)
            if (current_[b] != 0.0f || target_[b] != 0.0f) return false;
        return true;
    }

    void process(float* data, size_t frames) {
        size_t done = 0;
        while (done < frames) {
            const size_t n = std::min(kBlock, frames - done);
            for (size_t b = 0; b < N; ++b) {
                if (current_[b] != target_[b]) {
                    const float was = current_[b];
                    const float delta = target_[b] - was;
                    float step = delta * approach_;
                    if (maxStep_ > 0.0f) step = std::clamp(step, -maxStep_, maxStep_);
                    current_[b] = std::fabs(delta) <= 1e-4f || std::fabs(step) >= std::fabs(delta) ? target_[b] : was + step;
                    if (was == 0.0f) {
                        left_[b].reset();
                        right_[b].reset();
                    }
                    refresh(b);
                }
                if (current_[b] == 0.0f) continue;
                float* p = data + done * 2;
                for (size_t i = 0; i < n; ++i) {
                    p[2 * i] = left_[b].tick(p[2 * i]);
                    p[2 * i + 1] = right_[b].tick(p[2 * i + 1]);
                }
            }
            done += n;
        }
    }

    BiquadCoeffs coeffs(size_t b, float gainDb) const {
        return BiquadCoeffs::peaking(rate_, freqs_[b], q_, gainDb);
    }

private:
    void refresh(size_t b) {
        const BiquadCoeffs c = BiquadCoeffs::peaking(rate_, freqs_[b], q_, current_[b]);
        left_[b].setCoeffs(c);
        right_[b].setCoeffs(c);
    }

    std::array<double, N> freqs_{};
    double q_ = 1.0;
    double rate_ = 48000.0;
    float maxStep_ = 0.0f;
    float approach_ = 1.0f;
    std::array<float, N> target_{};
    std::array<float, N> current_{};
    std::array<Biquad, N> left_, right_;
};

}
