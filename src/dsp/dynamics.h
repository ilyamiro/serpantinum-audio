#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "params.h"

namespace sa {

inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

class Compressor {
public:
    struct Settings {
        float thresholdDb = -26.0f;
        float ratio = 2.8f;
        float kneeDb = 20.0f;
        float attackMs = 14.0f;
        float releaseMs = 200.0f;
    };

    void setSampleRate(double rate) {
        rate_ = rate;
        detectCoef_ = coefFor(kDetectMs);
        configure();
    }

    void set(const Settings& s) {
        settings_ = s;
        configure();
    }

    void process(float* data, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            const float l = data[2 * i], r = data[2 * i + 1];
            power_ += detectCoef_ * (0.5f * (l * l + r * r) - power_);
            const float levelDb = 10.0f * std::log10(power_ + 1e-12f);
            const float wantDb = reductionDb(levelDb);
            const float coef = wantDb > reductionState_ ? attackCoef_ : releaseCoef_;
            reductionState_ += coef * (wantDb - reductionState_);
            const float gain = dbToLin(-reductionState_);
            data[2 * i] = l * gain;
            data[2 * i + 1] = r * gain;
        }
    }

    void reset() {
        power_ = 0.0f;
        reductionState_ = 0.0f;
    }

private:
    static constexpr float kDetectMs = 10.0f;

    float coefFor(float ms) const {
        return 1.0f - std::exp(-1.0f / (0.001f * ms * static_cast<float>(rate_)));
    }

    void configure() {
        attackCoef_ = coefFor(std::max(settings_.attackMs, 0.1f));
        releaseCoef_ = coefFor(std::max(settings_.releaseMs, 1.0f));
    }

    float reductionDb(float levelDb) const {
        const float over = levelDb - settings_.thresholdDb;
        const float half = settings_.kneeDb * 0.5f;
        const float slope = 1.0f - 1.0f / settings_.ratio;
        if (over <= -half) return 0.0f;
        if (over >= half) return over * slope;
        const float x = over + half;
        return slope * x * x / (2.0f * settings_.kneeDb);
    }

    double rate_ = 48000.0;
    Settings settings_;
    float detectCoef_ = 0.0f, attackCoef_ = 0.0f, releaseCoef_ = 0.0f;
    float power_ = 0.0f;
    float reductionState_ = 0.0f;
};

class Limiter {
public:
    static constexpr float kThresholdDb = -1.0f;
    static constexpr float kSafetyThresholdDb = -0.1f;
    static constexpr float kLookaheadMs = 5.0f;
    static constexpr float kReleaseMs = 50.0f;

    void setSampleRate(double rate) {
        rate_ = rate;
        look_ = std::max<size_t>(2, static_cast<size_t>(kLookaheadMs * 0.001 * rate));
        releaseCoef_ = 1.0f - std::exp(-1.0f / (0.001f * kReleaseMs * static_cast<float>(rate)));
        threshold_ = dbToLin(kThresholdDb);
        size_t cap = 1;
        while (cap < look_ + 1) cap <<= 1;
        mask_ = cap - 1;
        delay_.assign(cap * 2, 0.0f);
        dqIndex_.assign(cap, 0);
        dqValue_.assign(cap, 1.0f);
        box_.assign(cap, 1.0f);
        reset();
    }

    void setThresholdDb(float db) { threshold_ = dbToLin(db); }

    size_t latencyFrames() const { return look_ - 1; }

    void process(float* data, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            const float l = data[2 * i], r = data[2 * i + 1];
            const float peak = std::max(std::fabs(l), std::fabs(r));
            const float raw = peak > threshold_ ? threshold_ / peak : 1.0f;

            while (dqCount_ > 0 && dqValue_[(dqHead_ + dqCount_ - 1) & mask_] >= raw) --dqCount_;
            const size_t tail = (dqHead_ + dqCount_) & mask_;
            dqIndex_[tail] = pos_;
            dqValue_[tail] = raw;
            ++dqCount_;
            while (pos_ - dqIndex_[dqHead_] >= look_) {
                dqHead_ = (dqHead_ + 1) & mask_;
                --dqCount_;
            }
            const float windowMin = dqValue_[dqHead_];

            held_ = std::min(windowMin, held_ + (1.0f - held_) * releaseCoef_);

            boxSum_ += held_ - box_[(pos_ - look_) & mask_];
            box_[pos_ & mask_] = held_;
            const float gain = std::min(1.0f, boxSum_ / static_cast<float>(look_));

            const size_t slot = pos_ & mask_;
            const size_t outSlot = (pos_ - (look_ - 1)) & mask_;
            delay_[2 * slot] = l;
            delay_[2 * slot + 1] = r;
            float ol = delay_[2 * outSlot] * gain;
            float orr = delay_[2 * outSlot + 1] * gain;
            ol = std::clamp(ol, -threshold_, threshold_);
            orr = std::clamp(orr, -threshold_, threshold_);
            data[2 * i] = ol;
            data[2 * i + 1] = orr;
            ++pos_;
        }
    }

    void reset() {
        std::fill(delay_.begin(), delay_.end(), 0.0f);
        std::fill(box_.begin(), box_.end(), 1.0f);
        boxSum_ = static_cast<float>(look_);
        held_ = 1.0f;
        dqHead_ = dqCount_ = 0;
        pos_ = look_;
    }

private:
    double rate_ = 48000.0;
    size_t look_ = 240, mask_ = 255, pos_ = 0;
    float releaseCoef_ = 0.0f, threshold_ = 0.89f, held_ = 1.0f, boxSum_ = 0.0f;
    std::vector<float> delay_, box_, dqValue_;
    std::vector<size_t> dqIndex_;
    size_t dqHead_ = 0, dqCount_ = 0;
};

}
