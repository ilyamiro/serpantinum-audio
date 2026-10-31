#pragma once

#include <algorithm>
#include <cstddef>

namespace sa {

class StereoStage {
public:
    void set(bool mono, bool swap, float balance) {
        mono_ = mono;
        swap_ = swap;
        balance_ = std::clamp(balance, -1.0f, 1.0f);
    }

    void process(float* data, size_t frames) {
        const float targetL = (!mono_ && balance_ > 0.0f) ? 1.0f - balance_ : 1.0f;
        const float targetR = (!mono_ && balance_ < 0.0f) ? 1.0f + balance_ : 1.0f;
        const float stepL = (targetL - gainL_) / static_cast<float>(frames ? frames : 1);
        const float stepR = (targetR - gainR_) / static_cast<float>(frames ? frames : 1);
        for (size_t i = 0; i < frames; ++i) {
            float l = data[2 * i], r = data[2 * i + 1];
            if (mono_) {
                l = r = 0.5f * (l + r);
            } else if (swap_) {
                std::swap(l, r);
            }
            gainL_ += stepL;
            gainR_ += stepR;
            data[2 * i] = l * gainL_;
            data[2 * i + 1] = r * gainR_;
        }
        gainL_ = targetL;
        gainR_ = targetR;
    }

private:
    bool mono_ = false, swap_ = false;
    float balance_ = 0.0f;
    float gainL_ = 1.0f, gainR_ = 1.0f;
};

}
