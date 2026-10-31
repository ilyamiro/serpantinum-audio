#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

#include "params.h"
#include "peaking_bank.h"

namespace sa {

class Equalizer {
public:
    static constexpr std::array<double, kEqBands> kFreqs = {
        32, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    static constexpr double kQ = 1.0;
    static constexpr float kMaxStepDb = 0.25f;

    void setSampleRate(double rate) { bank_.configure(kFreqs, kQ, rate); }

    void setGains(const std::array<float, kEqBands>& db) {
        std::array<float, kEqBands> clamped{};
        for (int b = 0; b < kEqBands; ++b) clamped[b] = std::clamp(db[b], -24.0f, 24.0f);
        bank_.setTarget(clamped, kMaxStepDb, 1.0f);
    }

    bool idle() const { return bank_.idle(); }

    void process(float* data, size_t frames) { bank_.process(data, frames); }

private:
    PeakingBank<kEqBands> bank_;
};

}
