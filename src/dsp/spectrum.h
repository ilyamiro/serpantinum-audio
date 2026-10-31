#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "fft.h"

namespace sa {

class ThirdOctaveAnalyzer {
public:
    static constexpr int kFirstBand = -14;
    static constexpr int kLastBand = 12;
    static constexpr int kBands = kLastBand - kFirstBand + 1;
    static constexpr size_t kFftSize = 16384;
    static constexpr size_t kHop = 8192;

    using Bands = std::array<float, kBands>;

    ThirdOctaveAnalyzer()
        : fft_(kFftSize), window_(kFftSize), left_(kFftSize, 0.0f), right_(kFftSize, 0.0f), re_(kFftSize), im_(kFftSize), power_(kFftSize / 2) {
        double sum = 0.0;
        for (size_t i = 0; i < kFftSize; ++i) {
            window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(kFftSize)));
            sum += static_cast<double>(window_[i]) * window_[i];
        }
        norm_ = static_cast<float>(1.0 / (static_cast<double>(kFftSize) * sum));
    }

    static double bandCenter(int band) { return 1000.0 * std::pow(2.0, static_cast<double>(band + kFirstBand) / 3.0); }

    static double bandOffsetOctaves(int band) { return static_cast<double>(band + kFirstBand) / 3.0; }

    void setSampleRate(double rate) {
        rate_ = rate;
        reset();
    }

    void reset() {
        std::fill(left_.begin(), left_.end(), 0.0f);
        std::fill(right_.begin(), right_.end(), 0.0f);
        write_ = 0;
        sinceBlock_ = 0;
        filled_ = 0;
    }

    template <class OnBlock>
    void push(const float* data, size_t frames, OnBlock&& onBlock) {
        for (size_t i = 0; i < frames; ++i) {
            left_[write_] = data[2 * i];
            right_[write_] = data[2 * i + 1];
            write_ = (write_ + 1) % kFftSize;
            if (filled_ < kFftSize) ++filled_;
            if (++sinceBlock_ >= kHop) {
                sinceBlock_ = 0;
                if (filled_ >= kFftSize) {
                    Bands bands;
                    float total = 0.0f;
                    analyze(bands, total);
                    onBlock(bands, total);
                }
            }
        }
    }

private:
    void analyze(Bands& bands, float& total) {
        for (size_t i = 0; i < kFftSize; ++i) {
            const size_t src = (write_ + i) % kFftSize;
            re_[i] = left_[src] * window_[i];
            im_[i] = right_[src] * window_[i];
        }
        fft_.forward(re_.data(), im_.data());

        const size_t half = kFftSize / 2;
        std::vector<float>& power = power_;
        std::fill(power.begin(), power.end(), 0.0f);
        total = 0.0f;
        for (size_t k = 1; k < half; ++k) {
            const size_t m = kFftSize - k;
            const float lr = 0.5f * (re_[k] + re_[m]);
            const float li = 0.5f * (im_[k] - im_[m]);
            const float rr = 0.5f * (im_[k] + im_[m]);
            const float ri = -0.5f * (re_[k] - re_[m]);
            power[k] = (lr * lr + li * li + rr * rr + ri * ri) * norm_;
            total += power[k];
        }

        const double df = rate_ / static_cast<double>(kFftSize);
        for (int b = 0; b < kBands; ++b) {
            const double fc = bandCenter(b);
            const double lo = fc * std::pow(2.0, -1.0 / 6.0);
            const double hi = fc * std::pow(2.0, 1.0 / 6.0);
            const size_t first = static_cast<size_t>(std::max(1.0, std::ceil(lo / df)));
            const size_t last = static_cast<size_t>(std::min(static_cast<double>(half - 1), std::floor(hi / df)));
            double sum = 0.0;
            size_t count = 0;
            for (size_t k = first; k <= last; ++k, ++count) sum += power[k];
            if (count == 0) {
                const size_t nearest = static_cast<size_t>(std::clamp(std::round(fc / df), 1.0, static_cast<double>(half - 1)));
                sum = power[nearest];
                count = 1;
            }
            bands[b] = static_cast<float>(sum / static_cast<double>(count) * (hi - lo) / df);
        }
    }

    Fft fft_;
    double rate_ = 48000.0;
    float norm_ = 1.0f;
    std::vector<float> window_, left_, right_, re_, im_, power_;
    size_t write_ = 0;
    size_t sinceBlock_ = 0;
    size_t filled_ = 0;
};

}
