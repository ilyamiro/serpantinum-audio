#pragma once

#include <cstddef>

#include "dynamic_eq.h"
#include "dynamics.h"
#include "equalizer.h"
#include "params.h"
#include "stereo.h"

namespace sa {

class Chain {
public:
    void setSampleRate(double rate) {
        eq_.setSampleRate(rate);
        dynEq_.setSampleRate(rate);
        comp_.setSampleRate(rate);
        limiter_.setSampleRate(rate);
    }

    void setParams(const Params& p) {
        params_ = p;
        eq_.setGains(p.dynamicEq ? std::array<float, kEqBands>{} : p.gains);
        dynEq_.setEnabled(p.dynamicEq);
        stereo_.set(p.mono, p.swap, p.balance);
        limiter_.setThresholdDb(p.dynamics != Dynamics::Off ? Limiter::kThresholdDb : Limiter::kSafetyThresholdDb);
        if (p.dynamics == Dynamics::Night) {
            comp_.set({-36.0f, 6.0f, 28.0f, 6.0f, 320.0f});
        } else {
            comp_.set({-26.0f, 2.8f, 20.0f, 14.0f, 200.0f});
        }
    }

    void process(float* data, size_t frames) {
        if (params_.bypass) return;
        eq_.process(data, frames);
        dynEq_.process(data, frames);
        stereo_.process(data, frames);
        if (params_.dynamics != Dynamics::Off) comp_.process(data, frames);
        if (params_.dynamics != Dynamics::Off || params_.dynamicEq) limiter_.process(data, frames);
    }

private:
    Params params_;
    Equalizer eq_;
    DynamicEq dynEq_;
    StereoStage stereo_;
    Compressor comp_;
    Limiter limiter_;
};

}
