#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "biquad.h"
#include "params.h"
#include "peaking_bank.h"
#include "spectrum.h"

namespace sa {

class DynamicEq {
public:
    static constexpr int kBank = 17;
    static constexpr int kBandCount = ThirdOctaveAnalyzer::kBands;
    static constexpr int kGrid = 54;

    static constexpr float kTargetSlopeDbPerOctave = -2.5f;
    static constexpr float kDeadzoneDb = 2.5f;
    static constexpr float kStrength = 0.5f;
    static constexpr float kCutLimitDb = 6.0f;
    static constexpr float kBoostLimitDb = 3.0f;
    static constexpr float kBassPreferenceDb = 1.0f;
    static constexpr double kBassFullHz = 80.0;
    static constexpr double kBassZeroHz = 250.0;
    static constexpr float kMakeupMinDb = -6.0f;
    static constexpr float kMakeupMaxDb = 3.0f;
    static constexpr float kGateAbsoluteDb = -70.0f;
    static constexpr float kGateRelativeDb = -10.0f;
    static constexpr float kSlowSeconds = 8.0f;
    static constexpr float kFastSeconds = 1.5f;
    static constexpr float kMeanPowerSeconds = 30.0f;
    static constexpr float kChangeLowDb = 3.0f;
    static constexpr float kChangeHighDb = 8.0f;
    static constexpr float kCatchUp = 0.3f;
    static constexpr float kWarmupSeconds = 4.0f;
    static constexpr float kSlewSeconds = 0.25f;
    static constexpr double kBankQ = 2.0;
    static constexpr float kFitRidge = 0.02f;
    static constexpr float kFitSmooth = 0.1f;
    static constexpr float kFitClampLow = -8.0f;
    static constexpr float kFitClampHigh = 4.0f;

    static float weightAt(double freq) {
        constexpr double kLowZero = 50.0, kLowFull = 100.0, kHighFull = 10000.0, kHighZero = 16000.0;
        if (freq <= kLowZero || freq >= kHighZero) return 0.0f;
        if (freq < kLowFull) return static_cast<float>(0.5 - 0.5 * std::cos(M_PI * std::log2(freq / kLowZero) / std::log2(kLowFull / kLowZero)));
        if (freq <= kHighFull) return 1.0f;
        return static_cast<float>(0.5 + 0.5 * std::cos(M_PI * std::log2(freq / kHighFull) / std::log2(kHighZero / kHighFull)));
    }

    static float bassPreference(double freq) {
        if (freq <= kBassFullHz) return kBassPreferenceDb;
        if (freq >= kBassZeroHz) return 0.0f;
        return kBassPreferenceDb * static_cast<float>(0.5 + 0.5 * std::cos(M_PI * std::log2(freq / kBassFullHz) / std::log2(kBassZeroHz / kBassFullHz)));
    }

    static float shapeCorrection(float deviationDb) {
        const float sq = deviationDb * deviationDb;
        const float soft = deviationDb * sq / (sq + kDeadzoneDb * kDeadzoneDb);
        const float wanted = -kStrength * soft;
        return wanted >= 0.0f ? kBoostLimitDb * std::tanh(wanted / kBoostLimitDb) : kCutLimitDb * std::tanh(wanted / kCutLimitDb);
    }

    void setSampleRate(double rate) {
        rate_ = rate;
        analyzer_.setSampleRate(rate);
        std::array<double, kBank> freqs{};
        for (int j = 0; j < kBank; ++j) freqs[j] = 1000.0 * std::pow(2.0, static_cast<double>(j - 8) / 2.0);
        freqs_ = freqs;
        bank_.configure(freqs, kBankQ, rate);
        approach_ = 1.0f - std::exp(-static_cast<float>(PeakingBank<kBank>::kBlock) / (kSlewSeconds * static_cast<float>(rate)));
        prepareFit();
        prepareWeights();
        reset();
    }

    void setEnabled(bool on) {
        if (on == enabled_) return;
        enabled_ = on;
        if (on) {
            resetAnalysis();
        } else {
            gains_.fill(0.0f);
            makeupTarget_ = 1.0f;
            bank_.setTarget(gains_, 0.0f, approach_);
        }
    }

    void reset() {
        resetAnalysis();
        gains_.fill(0.0f);
        bank_.setTarget(gains_, 0.0f, approach_);
        makeup_ = makeupTarget_ = 1.0f;
    }

    void process(float* data, size_t frames) {
        if (!enabled_) {
            if (!bank_.idle()) bank_.process(data, frames);
            applyMakeup(data, frames);
            return;
        }
        analyzer_.push(data, frames, [this](const ThirdOctaveAnalyzer::Bands& bands, float total) { onBlock(bands, total); });
        bank_.process(data, frames);
        applyMakeup(data, frames);
    }

    const std::array<float, kBank>& gains() const { return gains_; }
    const std::array<float, kBank>& appliedGains() const { return bank_.current(); }
    float makeupDb() const { return 20.0f * std::log10(makeupTarget_); }
    float changeScore() const { return change_; }
    static std::array<double, kBank> bankFrequencies() {
        std::array<double, kBank> f{};
        for (int j = 0; j < kBank; ++j) f[j] = 1000.0 * std::pow(2.0, static_cast<double>(j - 8) / 2.0);
        return f;
    }

private:
    void resetAnalysis() {
        analyzer_.reset();
        slow_.fill(0.0f);
        fast_.fill(0.0f);
        meanPower_ = 0.0f;
        blocks_ = 0;
        change_ = 0.0f;
    }

    void prepareWeights() {
        for (int k = 0; k < kBandCount; ++k) {
            const double fc = ThirdOctaveAnalyzer::bandCenter(k);
            weights_[k] = weightAt(fc);
            const BiquadCoeffs shelf = BiquadCoeffs::highShelf(rate_, 1681.97, 0.7072, 3.9998);
            const BiquadCoeffs rlb = BiquadCoeffs::highpass(rate_, 38.13, 0.5003);
            const double db = shelf.magnitudeDb(rate_, fc) + rlb.magnitudeDb(rate_, fc);
            kWeight_[k] = static_cast<float>(std::pow(10.0, db / 10.0));
        }
    }

    void prepareFit() {
        for (int i = 0; i < kGrid; ++i) grid_[i] = 40.0 * std::pow(2.0, static_cast<double>(i) / 6.0);
        std::array<std::array<double, kBank>, kGrid> a{};
        for (int j = 0; j < kBank; ++j) {
            const BiquadCoeffs unit = BiquadCoeffs::peaking(rate_, freqs_[j], kBankQ, 1.0);
            for (int i = 0; i < kGrid; ++i) a[i][j] = unit.magnitudeDb(rate_, grid_[i]);
        }
        unit_ = a;
        for (int i = 0; i < kGrid; ++i) fitWeight_[i] = std::max(0.1f, weightAt(grid_[i]));

        std::array<std::array<double, kBank>, kBank> m{};
        for (int p = 0; p < kBank; ++p)
            for (int q = 0; q < kBank; ++q) {
                double acc = 0.0;
                for (int i = 0; i < kGrid; ++i) acc += fitWeight_[i] * a[i][p] * a[i][q];
                m[p][q] = acc;
            }
        for (int p = 0; p < kBank; ++p) m[p][p] += kFitRidge;
        for (int p = 0; p + 1 < kBank; ++p) {
            m[p][p] += kFitSmooth;
            m[p + 1][p + 1] += kFitSmooth;
            m[p][p + 1] -= kFitSmooth;
            m[p + 1][p] -= kFitSmooth;
        }
        chol_ = {};
        for (int i = 0; i < kBank; ++i) {
            for (int j = 0; j <= i; ++j) {
                double sum = m[i][j];
                for (int k = 0; k < j; ++k) sum -= chol_[i][k] * chol_[j][k];
                if (i == j) chol_[i][j] = std::sqrt(std::max(sum, 1e-9));
                else chol_[i][j] = sum / chol_[j][j];
            }
        }
    }

    void solve(const std::array<double, kBank>& rhs, std::array<double, kBank>& out) const {
        std::array<double, kBank> y{};
        for (int i = 0; i < kBank; ++i) {
            double sum = rhs[i];
            for (int k = 0; k < i; ++k) sum -= chol_[i][k] * y[k];
            y[i] = sum / chol_[i][i];
        }
        for (int i = kBank - 1; i >= 0; --i) {
            double sum = y[i];
            for (int k = i + 1; k < kBank; ++k) sum -= chol_[k][i] * out[k];
            out[i] = sum / chol_[i][i];
        }
    }

    void projectResidual(const std::array<double, kGrid>& residual, std::array<double, kBank>& rhs) const {
        for (int j = 0; j < kBank; ++j) {
            double acc = 0.0;
            for (int i = 0; i < kGrid; ++i) acc += fitWeight_[i] * unit_[i][j] * residual[i];
            rhs[j] = acc;
        }
    }

    void fitGains(const std::array<float, kBandCount>& desired, std::array<float, kBank>& out) const {
        std::array<double, kGrid> target{};
        for (int i = 0; i < kGrid; ++i) {
            const double pos = std::log2(grid_[i] / 1000.0) * 3.0 - ThirdOctaveAnalyzer::kFirstBand;
            if (pos <= 0.0) target[i] = desired[0];
            else if (pos >= kBandCount - 1) target[i] = desired[kBandCount - 1];
            else {
                const int lo = static_cast<int>(pos);
                const double t = pos - lo;
                target[i] = desired[lo] * (1.0 - t) + desired[lo + 1] * t;
            }
        }
        std::array<double, kBank> g{}, rhs{}, step{};
        projectResidual(target, rhs);
        solve(rhs, g);
        for (int iter = 0; iter < 2; ++iter) {
            std::array<double, kGrid> residual{};
            for (int i = 0; i < kGrid; ++i) {
                double h = 0.0;
                for (int j = 0; j < kBank; ++j)
                    h += BiquadCoeffs::peaking(rate_, freqs_[j], kBankQ, g[j]).magnitudeDb(rate_, grid_[i]);
                residual[i] = target[i] - h;
            }
            projectResidual(residual, rhs);
            solve(rhs, step);
            for (int j = 0; j < kBank; ++j) g[j] = std::clamp(g[j] + step[j], static_cast<double>(kFitClampLow), static_cast<double>(kFitClampHigh));
        }
        for (int j = 0; j < kBank; ++j) out[j] = static_cast<float>(std::clamp(g[j], static_cast<double>(kFitClampLow), static_cast<double>(kFitClampHigh)));
    }

    float realizedDb(const std::array<float, kBank>& g, double freq) const {
        double h = 0.0;
        for (int j = 0; j < kBank; ++j) h += BiquadCoeffs::peaking(rate_, freqs_[j], kBankQ, g[j]).magnitudeDb(rate_, freq);
        return static_cast<float>(h);
    }

    void onBlock(const ThirdOctaveAnalyzer::Bands& bands, float total) {
        const float totalDb = 10.0f * std::log10(total + 1e-20f);
        if (totalDb < kGateAbsoluteDb) return;
        if (meanPower_ > 0.0f && totalDb < 10.0f * std::log10(meanPower_) + kGateRelativeDb) return;

        const float hop = static_cast<float>(ThirdOctaveAnalyzer::kHop);
        const float rate = static_cast<float>(rate_);
        meanPower_ = meanPower_ == 0.0f ? total : meanPower_ + (1.0f - std::exp(-hop / (kMeanPowerSeconds * rate))) * (total - meanPower_);
        ++blocks_;
        const float uniform = 1.0f / static_cast<float>(blocks_);
        const float alphaSlow = std::max(uniform, 1.0f - std::exp(-hop / (kSlowSeconds * rate)));
        const float alphaFast = std::max(uniform, 1.0f - std::exp(-hop / (kFastSeconds * rate)));
        for (int k = 0; k < kBandCount; ++k) {
            slow_[k] += alphaSlow * (bands[k] - slow_[k]);
            fast_[k] += alphaFast * (bands[k] - fast_[k]);
        }

        float weightSum = 0.0f, meanDiff = 0.0f;
        std::array<float, kBandCount> diff{};
        for (int k = 0; k < kBandCount; ++k) {
            diff[k] = 10.0f * std::log10((fast_[k] + 1e-20f) / (slow_[k] + 1e-20f));
            meanDiff += weights_[k] * diff[k];
            weightSum += weights_[k];
        }
        meanDiff /= std::max(weightSum, 1e-6f);
        float rms = 0.0f;
        for (int k = 0; k < kBandCount; ++k) rms += weights_[k] * (diff[k] - meanDiff) * (diff[k] - meanDiff);
        change_ = std::sqrt(rms / std::max(weightSum, 1e-6f));
        const float blend = std::clamp((change_ - kChangeLowDb) / (kChangeHighDb - kChangeLowDb), 0.0f, 1.0f);

        std::array<float, kBandCount> effective{};
        for (int k = 0; k < kBandCount; ++k) {
            effective[k] = (1.0f - blend) * slow_[k] + blend * fast_[k];
            slow_[k] += blend * kCatchUp * (fast_[k] - slow_[k]);
        }

        std::array<float, kBandCount> deviation{};
        float mean = 0.0f;
        for (int k = 0; k < kBandCount; ++k) {
            const float level = 10.0f * std::log10(effective[k] + 1e-20f);
            deviation[k] = level - kTargetSlopeDbPerOctave * static_cast<float>(ThirdOctaveAnalyzer::bandOffsetOctaves(k));
            mean += weights_[k] * deviation[k];
        }
        mean /= std::max(weightSum, 1e-6f);

        std::array<float, kBandCount> smooth{};
        for (int k = 0; k < kBandCount; ++k) {
            const float prev = deviation[std::max(0, k - 1)];
            const float next = deviation[std::min(kBandCount - 1, k + 1)];
            smooth[k] = 0.25f * prev + 0.5f * deviation[k] + 0.25f * next - mean;
        }

        const float warm = std::min(1.0f, static_cast<float>(blocks_) * hop / (kWarmupSeconds * rate));
        std::array<float, kBandCount> desired{};
        for (int k = 0; k < kBandCount; ++k)
            desired[k] = (shapeCorrection(smooth[k]) * weights_[k] + bassPreference(ThirdOctaveAnalyzer::bandCenter(k))) * warm;

        std::array<float, kBank> fitted{};
        fitGains(desired, fitted);
        gains_ = fitted;

        float num = 0.0f, den = 0.0f;
        for (int k = 0; k < kBandCount; ++k) {
            const float response = realizedDb(fitted, ThirdOctaveAnalyzer::bandCenter(k));
            const float p = effective[k] * kWeight_[k];
            num += p * std::pow(10.0f, response / 10.0f);
            den += p;
        }
        const float deltaDb = 10.0f * std::log10(std::max(num, 1e-20f) / std::max(den, 1e-20f));
        makeupTarget_ = std::pow(10.0f, std::clamp(-deltaDb, kMakeupMinDb, kMakeupMaxDb) / 20.0f);

        bank_.setTarget(gains_, 0.0f, approach_);
    }

    void applyMakeup(float* data, size_t frames) {
        if (makeup_ == makeupTarget_) {
            if (makeup_ != 1.0f)
                for (size_t i = 0; i < frames * 2; ++i) data[i] *= makeup_;
            return;
        }
        const float coef = 1.0f - std::exp(-1.0f / (kSlewSeconds * static_cast<float>(rate_)));
        for (size_t i = 0; i < frames; ++i) {
            makeup_ += coef * (makeupTarget_ - makeup_);
            data[2 * i] *= makeup_;
            data[2 * i + 1] *= makeup_;
        }
        if (std::fabs(makeup_ - makeupTarget_) < 1e-5f) makeup_ = makeupTarget_;
    }

    double rate_ = 48000.0;
    bool enabled_ = false;
    float approach_ = 0.01f;
    float meanPower_ = 0.0f;
    float change_ = 0.0f;
    float makeup_ = 1.0f;
    float makeupTarget_ = 1.0f;
    size_t blocks_ = 0;

    ThirdOctaveAnalyzer analyzer_;
    PeakingBank<kBank> bank_;
    std::array<double, kBank> freqs_{};
    std::array<double, kGrid> grid_{};
    std::array<std::array<double, kBank>, kGrid> unit_{};
    std::array<std::array<double, kBank>, kBank> chol_{};
    std::array<float, kGrid> fitWeight_{};
    std::array<float, kBandCount> weights_{};
    std::array<float, kBandCount> kWeight_{};
    std::array<float, kBandCount> slow_{};
    std::array<float, kBandCount> fast_{};
    std::array<float, kBank> gains_{};
};

}
