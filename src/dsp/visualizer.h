// CavaCore and the output stage of Visualizer are ported from cava 1.0.0 (cavacore.c and cava.c):
//
// Copyright (c) 2022 Karl Stavestrand <karl@stavestrand.no>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fft.h"

namespace sa {

enum class MonoOption { Average, Left, Right };

struct VisualizerOptions {
    int bars = 64;
    int framerate = 60;
    int sensitivity = 100;
    bool autosens = true;
    int noiseReduction = 77;
    int lowerCutoff = 50;
    int higherCutoff = 8000;
    bool stereo = true;
    MonoOption monoOption = MonoOption::Average;
    bool reverse = false;
    double monstercat = 0.0;
    bool waves = false;
    std::vector<double> eq;
    int sampleRate = 48000;
    int range = 1000;
};

// cavacore: two Hann-windowed FFTs, a long one for the
// bars below 100 Hz and a short one for the rest, log-spaced bars, gravity falloff, integral smoothing
// and automatic sensitivity. The input is interleaved stereo in the 16-bit range.
class CavaCore {
public:
    static constexpr int kChannels = 2;

    bool init(int bars, int rate, bool autosens, double noiseReduction, int lowCutOff, int highCutOff, std::string& error) {
        int fftSize = 512;
        if (rate > 8125 && rate <= 16250) fftSize *= 2;
        else if (rate > 16250 && rate <= 32500) fftSize *= 4;
        else if (rate > 32500 && rate <= 75000) fftSize *= 8;
        else if (rate > 75000 && rate <= 150000) fftSize *= 16;
        else if (rate > 150000 && rate <= 300000) fftSize *= 32;
        else if (rate > 300000) fftSize *= 64;

        if (rate < 1 || rate > 384000) return fail(error, "illegal sample rate: " + std::to_string(rate));
        if (bars < 1) return fail(error, "number of bars must be a positive integer");
        if (bars > fftSize / 2 + 1)
            return fail(error, "number of bars can't be more than " + std::to_string(fftSize / 2 + 1) + " at " +
                                   std::to_string(rate) + " Hz");
        if (lowCutOff < 1 || highCutOff < 1) return fail(error, "cutoff frequencies must be positive");
        if (lowCutOff >= highCutOff) return fail(error, "higher cutoff must be higher than lower cutoff");
        if (highCutOff > rate / 2) return fail(error, "higher cutoff can't be higher than sample rate / 2");

        bars_ = bars;
        rate_ = rate;
        autosens_ = autosens;
        sens_ = 1.0;
        sensInit_ = true;
        framerate_ = 75.0;
        frameSkip_ = 1;
        noiseReduction_ = noiseReduction;

        bassSize_ = fftSize * 2;
        size_ = fftSize;
        inputSize_ = bassSize_ * kChannels;
        input_.assign(inputSize_, 0.0);
        bassWindow_.resize(bassSize_);
        window_.resize(size_);
        for (int i = 0; i < bassSize_; ++i) bassWindow_[i] = 0.5 * (1 - std::cos(2 * M_PI * i / (bassSize_ - 1)));
        for (int i = 0; i < size_; ++i) window_[i] = 0.5 * (1 - std::cos(2 * M_PI * i / (size_ - 1)));
        bassFft_ = std::make_unique<BasicFft<double>>(bassSize_);
        fft_ = std::make_unique<BasicFft<double>>(size_);
        bassRe_.assign(bassSize_, 0.0);
        bassIm_.assign(bassSize_, 0.0);
        re_.assign(size_, 0.0);
        im_.assign(size_, 0.0);

        const int count = bars * kChannels;
        fall_.assign(count, 0.0);
        mem_.assign(count, 0.0);
        peak_.assign(count, 0.0);
        prev_.assign(count, 0.0);

        lower_.assign(bars + 1, 0);
        upper_.assign(bars + 1, 0);
        cutOff_.assign(bars + 1, 0.0f);
        eq_.assign(bars + 1, 0.0);

        const int bassCutOff = 100;
        const double frequencyConstant = std::log10(static_cast<float>(lowCutOff) / static_cast<float>(highCutOff)) /
                                         (1 / (static_cast<float>(bars) + 1) - 1);
        std::vector<float> relative(bars + 1, 0.0f);
        bassBars_ = 0;
        bool firstBar = true;
        const float minBandwidth = static_cast<float>(rate / bassSize_);

        for (int n = 0; n < bars + 1; ++n) {
            double coefficient = frequencyConstant * (-1);
            coefficient += (static_cast<float>(n) + 1) / (static_cast<float>(bars) + 1) * frequencyConstant;
            cutOff_[n] = static_cast<float>(highCutOff * std::pow(10, coefficient));
            if (n > 0 && cutOff_[n - 1] >= cutOff_[n]) cutOff_[n] = cutOff_[n - 1] + minBandwidth;

            relative[n] = cutOff_[n] / static_cast<float>(rate / 2);

            if (cutOff_[n] < bassCutOff) {
                lower_[n] = static_cast<int>(relative[n] * static_cast<float>(bassSize_ / 2));
                ++bassBars_;
                if (bassBars_ > 1) firstBar = false;
                if (lower_[n] > bassSize_ / 2) lower_[n] = bassSize_ / 2;
            } else {
                lower_[n] = static_cast<int>(std::ceil(relative[n] * static_cast<float>(size_ / 2)));
                if (n == bassBars_) {
                    firstBar = true;
                    if (n > 0) upper_[n - 1] = static_cast<int>(relative[n] * static_cast<float>(bassSize_ / 2) - 1);
                } else {
                    firstBar = false;
                }
                if (lower_[n] > size_ / 2) lower_[n] = size_ / 2;
            }

            if (n > 0) {
                if (!firstBar) {
                    upper_[n - 1] = lower_[n] - 1;
                    if (lower_[n] <= lower_[n - 1]) {
                        const int limit = n < bassBars_ ? bassSize_ / 2 + 1 : size_ / 2 + 1;
                        if (lower_[n - 1] + 1 < limit) {
                            lower_[n] = lower_[n - 1] + 1;
                            upper_[n - 1] = lower_[n] - 1;
                        }
                    }
                } else if (upper_[n - 1] < lower_[n - 1]) {
                    upper_[n - 1] = lower_[n - 1] + 1;
                }
            }

            if (n < bassBars_)
                relative[n] = static_cast<float>(lower_[n]) / (static_cast<float>(bassSize_) / 2);
            else
                relative[n] = static_cast<float>(lower_[n]) / (static_cast<float>(size_) / 2);
            cutOff_[n] = relative[n] * (static_cast<float>(rate) / 2);
        }

        for (int n = 0; n < bars; ++n) {
            eq_[n] = 1 / std::pow(2, 28);
            eq_[n] *= std::pow(cutOff_[n + 1], 0.85);
            eq_[n] /= std::log2(n < bassBars_ ? bassSize_ : size_);
            eq_[n] /= upper_[n] - lower_[n] + 1;
        }
        return true;
    }

    int inputSize() const { return inputSize_; }
    int bars() const { return bars_; }
    const std::vector<float>& cutOffFrequencies() const { return cutOff_; }

    // Writes bars() left channel values followed by bars() right channel values, lowest frequency first.
    void execute(const double* in, int newSamples, double* out) {
        if (newSamples > inputSize_) newSamples = inputSize_;

        bool silence = true;
        if (newSamples > 0) {
            framerate_ -= framerate_ / 64.0;
            framerate_ += static_cast<double>(rate_ * frameSkip_) / (newSamples / kChannels) / 64.0;
            frameSkip_ = 1;

            std::memmove(input_.data() + newSamples, input_.data(), sizeof(double) * (inputSize_ - newSamples));
            for (int n = 0; n < newSamples; ++n) {
                input_[newSamples - n - 1] = in[n];
                if (in[n]) silence = false;
            }
        } else {
            ++frameSkip_;
        }

        for (int n = 0; n < bassSize_; ++n) {
            bassRe_[n] = bassWindow_[n] * input_[n * 2 + 1];
            bassIm_[n] = bassWindow_[n] * input_[n * 2];
        }
        for (int n = 0; n < size_; ++n) {
            re_[n] = window_[n] * input_[n * 2 + 1];
            im_[n] = window_[n] * input_[n * 2];
        }
        bassFft_->forward(bassRe_.data(), bassIm_.data());
        fft_->forward(re_.data(), im_.data());

        for (int n = 0; n < bars_; ++n) {
            const bool bass = n < bassBars_;
            const std::vector<double>& re = bass ? bassRe_ : re_;
            const std::vector<double>& im = bass ? bassIm_ : im_;
            const int size = bass ? bassSize_ : size_;
            double left = 0, right = 0;
            for (int i = lower_[n]; i <= upper_[n]; ++i) {
                const int m = (size - i) % size;
                left += std::hypot(0.5 * (re[i] + re[m]), 0.5 * (im[i] - im[m]));
                right += std::hypot(0.5 * (im[i] + im[m]), 0.5 * (re[i] - re[m]));
            }
            out[n] = left * eq_[n];
            out[n + bars_] = right * eq_[n];
        }

        const int count = bars_ * kChannels;
        if (autosens_)
            for (int n = 0; n < count; ++n) out[n] *= sens_;

        bool overshoot = false;
        const double framerateMod = 66 / framerate_;
        const double gravityMod = std::pow(framerateMod, 2.5) * 2 / noiseReduction_;
        const double integralMod = std::pow(framerateMod, 0.1);

        for (int n = 0; n < count; ++n) {
            if (out[n] < prev_[n] && noiseReduction_ > 0.1) {
                out[n] = peak_[n] * (1.0 - (fall_[n] * fall_[n] * gravityMod));
                if (out[n] < 0.0) out[n] = 0.0;
                fall_[n] += 0.028;
            } else {
                peak_[n] = out[n];
                fall_[n] = 0.0;
            }
            prev_[n] = out[n];

            out[n] = mem_[n] * noiseReduction_ / integralMod + out[n];
            mem_[n] = out[n];
            if (autosens_ && out[n] > 1.0) {
                overshoot = true;
                out[n] = 1.0;
            }
        }

        if (autosens_) {
            if (overshoot) {
                sens_ = sens_ * (1 - (0.02 * framerateMod));
                sensInit_ = false;
            } else if (!silence) {
                sens_ = sens_ * (1 + (0.001 * framerateMod));
                if (sensInit_) sens_ = sens_ * (1 + (0.1 * framerateMod));
            }
        }
    }

private:
    static bool fail(std::string& error, const std::string& message) {
        error = message;
        return false;
    }

    int bars_ = 0, rate_ = 0, bassBars_ = 0, frameSkip_ = 1;
    int bassSize_ = 0, size_ = 0, inputSize_ = 0;
    bool autosens_ = true, sensInit_ = true;
    double sens_ = 1.0, framerate_ = 75.0, noiseReduction_ = 0.77;
    std::unique_ptr<BasicFft<double>> bassFft_, fft_;
    std::vector<double> input_, bassWindow_, window_, bassRe_, bassIm_, re_, im_;
    std::vector<double> fall_, mem_, peak_, prev_, eq_;
    std::vector<int> lower_, upper_;
    std::vector<float> cutOff_;
};

// The rest of cava's raw output path: sensitivity, clamping to the range, the [eq] section, monstercat
// smoothing and the channel layout. Stereo puts the left channel mirrored on the left half and the right
// channel on the right half, with the low frequencies in the centre.
class Visualizer {
public:
    bool init(const VisualizerOptions& o, std::string& error) {
        if (o.framerate < 1) return fail(error, "framerate can't be less than 1");
        if (o.sensitivity < 1) return fail(error, "sensitivity needs to be at least 1%");
        if (o.lowerCutoff > o.higherCutoff) return fail(error, "lower cutoff can't be higher than higher cutoff");
        if (o.range < 1) return fail(error, "range must be positive");
        if (o.stereo && o.bars % 2 != 0) return fail(error, "stereo output needs an even number of bars");
        if (o.bars < (o.stereo ? 2 : 1)) return fail(error, "too few bars");
        opt_ = o;
        if (opt_.lowerCutoff == 0) opt_.lowerCutoff = 1;
        sens_ = o.sensitivity / 100.0;
        perChannel_ = o.bars / (o.stereo ? 2 : 1);
        const double noise = std::clamp(o.noiseReduction / 100.0, 0.0, 1.0);
        if (!core_.init(perChannel_, o.sampleRate, o.autosens, noise, opt_.lowerCutoff, o.higherCutoff, error)) return false;
        coreOut_.assign(perChannel_ * CavaCore::kChannels, 0.0);
        left_.assign(perChannel_, 0.0f);
        right_.assign(perChannel_, 0.0f);
        bars_.assign(o.bars, 0);
        eqRatio_ = o.eq.empty() ? 0.0 : static_cast<double>(o.eq.size()) / static_cast<double>(perChannel_);
        return true;
    }

    int inputSize() const { return core_.inputSize(); }
    const std::vector<int>& bars() const { return bars_; }
    const VisualizerOptions& options() const { return opt_; }

    const std::vector<int>& execute(const double* in, int newSamples) {
        core_.execute(in, newSamples, coreOut_.data());
        const double range = opt_.range;
        for (double& v : coreOut_) {
            v *= sens_;
            if (v > 1.0) v = 1.0;
            else if (v < 0.0) v = 0.0;
            v *= range;
        }
        for (int n = 0; n < perChannel_; ++n) {
            const double gain = opt_.eq.empty() ? 1.0 : opt_.eq[static_cast<size_t>(std::floor(n * eqRatio_))];
            left_[n] = static_cast<float>(coreOut_[n] * gain);
            right_[n] = static_cast<float>(coreOut_[n + perChannel_] * gain);
        }
        if (opt_.monstercat != 0.0) {
            monstercat(left_);
            monstercat(right_);
        }

        const int total = opt_.bars;
        std::vector<float> raw(total);
        if (opt_.stereo) {
            const int half = total / 2;
            for (int n = 0; n < total; ++n) {
                if (n < half) raw[n] = opt_.reverse ? left_[n] : left_[half - n - 1];
                else raw[n] = opt_.reverse ? right_[total - n - 1] : right_[n - half];
            }
        } else {
            for (int n = 0; n < total; ++n) {
                float v = opt_.monoOption == MonoOption::Left    ? left_[n]
                          : opt_.monoOption == MonoOption::Right ? right_[n]
                                                                 : (left_[n] + right_[n]) / 2;
                raw[opt_.reverse ? total - n - 1 : n] = v;
            }
        }
        for (int n = 0; n < total; ++n) bars_[n] = std::min(static_cast<int>(raw[n]), opt_.range);
        return bars_;
    }

private:
    static bool fail(std::string& error, const std::string& message) {
        error = message;
        return false;
    }

    void monstercat(std::vector<float>& bars) const {
        const int count = static_cast<int>(bars.size());
        const double heightNormalizer = opt_.range > 1000 ? opt_.range / 912.76 : 1.0;
        if (opt_.waves) {
            for (int z = 0; z < count; ++z) {
                bars[z] = static_cast<float>(bars[z] / 1.25);
                for (int m = z - 1; m >= 0; --m) {
                    const int de = z - m;
                    bars[m] = static_cast<float>(std::max(bars[z] - heightNormalizer * std::pow(de, 2), static_cast<double>(bars[m])));
                }
                for (int m = z + 1; m < count; ++m) {
                    const int de = m - z;
                    bars[m] = static_cast<float>(std::max(bars[z] - heightNormalizer * std::pow(de, 2), static_cast<double>(bars[m])));
                }
            }
        } else if (opt_.monstercat > 0) {
            for (int z = 0; z < count; ++z) {
                for (int m = z - 1; m >= 0; --m) {
                    const int de = z - m;
                    bars[m] = static_cast<float>(std::max(bars[z] / std::pow(opt_.monstercat * 1.5, de), static_cast<double>(bars[m])));
                }
                for (int m = z + 1; m < count; ++m) {
                    const int de = m - z;
                    bars[m] = static_cast<float>(std::max(bars[z] / std::pow(opt_.monstercat * 1.5, de), static_cast<double>(bars[m])));
                }
            }
        }
    }

    VisualizerOptions opt_;
    CavaCore core_;
    double sens_ = 1.0;
    double eqRatio_ = 0.0;
    int perChannel_ = 0;
    std::vector<double> coreOut_;
    std::vector<float> left_, right_;
    std::vector<int> bars_;
};

}
