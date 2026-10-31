#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace sa {

class Fft {
public:
    explicit Fft(size_t n) : n_(n), cos_(n / 2), sin_(n / 2), rev_(n) {
        for (size_t i = 0; i < n / 2; ++i) {
            cos_[i] = std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n));
            sin_[i] = -std::sin(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n));
        }
        size_t bits = 0;
        while ((static_cast<size_t>(1) << bits) < n) ++bits;
        for (size_t i = 0; i < n; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b)
                if (i & (static_cast<size_t>(1) << b)) r |= static_cast<size_t>(1) << (bits - 1 - b);
            rev_[i] = r;
        }
    }

    size_t size() const { return n_; }

    void forward(float* re, float* im) const {
        for (size_t i = 0; i < n_; ++i) {
            const size_t j = rev_[i];
            if (j > i) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t half = len / 2;
            const size_t step = n_ / len;
            for (size_t start = 0; start < n_; start += len) {
                for (size_t k = 0; k < half; ++k) {
                    const double wr = cos_[k * step];
                    const double wi = sin_[k * step];
                    const size_t a = start + k;
                    const size_t b = a + half;
                    const double tr = wr * re[b] - wi * im[b];
                    const double ti = wr * im[b] + wi * re[b];
                    re[b] = static_cast<float>(re[a] - tr);
                    im[b] = static_cast<float>(im[a] - ti);
                    re[a] = static_cast<float>(re[a] + tr);
                    im[a] = static_cast<float>(im[a] + ti);
                }
            }
        }
    }

private:
    size_t n_;
    std::vector<double> cos_, sin_;
    std::vector<size_t> rev_;
};

}
