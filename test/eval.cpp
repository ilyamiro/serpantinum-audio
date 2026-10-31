#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/dsp/dynamic_eq.h"
#include "../src/dsp/dynamics.h"

namespace {

constexpr double kRate = 48000.0;
constexpr int kBands = sa::ThirdOctaveAnalyzer::kBands;

std::array<double, kBands> levels(const std::vector<float>& v, size_t fromFrame, size_t toFrame) {
    sa::ThirdOctaveAnalyzer analyzer;
    analyzer.setSampleRate(kRate);
    std::array<double, kBands> acc{};
    int blocks = 0;
    analyzer.push(v.data() + fromFrame * 2, toFrame - fromFrame, [&](const sa::ThirdOctaveAnalyzer::Bands& b, float total) {
        if (10.0 * std::log10(total + 1e-20) < -70.0) return;
        for (int k = 0; k < kBands; ++k) acc[k] += b[k];
        ++blocks;
    });
    std::array<double, kBands> db{};
    for (int k = 0; k < kBands; ++k) db[k] = 10.0 * std::log10(acc[k] / std::max(blocks, 1) + 1e-20);
    return db;
}

double deviation(const std::array<double, kBands>& l, std::array<double, kBands>* devOut = nullptr) {
    std::array<double, kBands> dev{};
    double mean = 0.0, wsum = 0.0;
    for (int k = 0; k < kBands; ++k) {
        const double w = sa::DynamicEq::weightAt(sa::ThirdOctaveAnalyzer::bandCenter(k));
        dev[k] = l[k] - sa::DynamicEq::kTargetSlopeDbPerOctave * sa::ThirdOctaveAnalyzer::bandOffsetOctaves(k);
        mean += w * dev[k];
        wsum += w;
    }
    mean /= wsum;
    double var = 0.0;
    for (int k = 0; k < kBands; ++k) {
        const double w = sa::DynamicEq::weightAt(sa::ThirdOctaveAnalyzer::bandCenter(k));
        dev[k] -= mean;
        var += w * dev[k] * dev[k];
    }
    if (devOut) *devOut = dev;
    return std::sqrt(var / wsum);
}

double kDb(const std::array<double, kBands>& l) {
    const sa::BiquadCoeffs shelf = sa::BiquadCoeffs::highShelf(kRate, 1681.97, 0.7072, 3.9998);
    const sa::BiquadCoeffs rlb = sa::BiquadCoeffs::highpass(kRate, 38.13, 0.5003);
    double sum = 0.0;
    for (int k = 0; k < kBands; ++k) {
        const double fc = sa::ThirdOctaveAnalyzer::bandCenter(k);
        sum += std::pow(10.0, (l[k] + shelf.magnitudeDb(kRate, fc) + rlb.magnitudeDb(kRate, fc)) / 10.0);
    }
    return 10.0 * std::log10(sum);
}

}

int main(int, char** argv) {
    std::vector<float> in;
    float buf[8192];
    size_t n;
    while ((n = std::fread(buf, sizeof(float), 8192, stdin)) > 0) in.insert(in.end(), buf, buf + n);
    const size_t frames = in.size() / 2;
    if (frames < static_cast<size_t>(kRate * 20)) {
        std::fprintf(stderr, "need at least 20 s of audio\n");
        return 1;
    }

    sa::DynamicEq eq;
    eq.setSampleRate(kRate);
    eq.setEnabled(true);
    sa::Limiter limiter;
    limiter.setSampleRate(kRate);
    limiter.setThresholdDb(sa::Limiter::kSafetyThresholdDb);
    auto out = in;
    auto eqOnly = in;

    std::vector<std::array<float, sa::DynamicEq::kBank>> log;
    double driftMax = 0.0;
    size_t nextLog = static_cast<size_t>(kRate) * 10;
    for (size_t off = 0; off < frames; off += 512) {
        const size_t n2 = std::min<size_t>(512, frames - off);
        eq.process(out.data() + off * 2, n2);
        std::copy(out.begin() + static_cast<long>(off * 2), out.begin() + static_cast<long>((off + n2) * 2), eqOnly.begin() + static_cast<long>(off * 2));
        limiter.process(out.data() + off * 2, n2);
        if (off >= nextLog) {
            log.push_back(eq.appliedGains());
            nextLog += static_cast<size_t>(kRate) * 10;
        }
    }
    for (size_t i = 1; i < log.size(); ++i)
        for (int j = 0; j < sa::DynamicEq::kBank; ++j) driftMax = std::max(driftMax, static_cast<double>(std::fabs(log[i][j] - log[i - 1][j])) / 10.0);

    const size_t from = frames / 3;
    const auto a = levels(in, from, frames), b = levels(out, from, frames), e = levels(eqOnly, from, frames);
    std::array<double, kBands> dev{};
    const double before = deviation(a, &dev), after = deviation(b);
    float peakIn = 0.0f, peakOut = 0.0f;
    for (size_t i = 0; i < in.size(); ++i) {
        peakIn = std::max(peakIn, std::fabs(in[i]));
        peakOut = std::max(peakOut, std::fabs(out[i]));
    }
    std::printf("%s\n", argv[0] ? "eval" : "");
    std::printf("duration            %.1f s\n", static_cast<double>(frames) / kRate);
    std::printf("deviation from target  before %.2f dB  after %.2f dB  (%.0f%% lower)\n", before, after, 100.0 * (1.0 - after / std::max(before, 1e-9)));
    std::printf("K-weighted loudness change  EQ only %+.2f dB   with limiter %+.2f dB\n", kDb(e) - kDb(a), kDb(b) - kDb(a));
    std::printf("peak  in %.3f  out %.3f (limit %.3f)\n", peakIn, peakOut, std::pow(10.0, sa::Limiter::kSafetyThresholdDb / 20.0));
    std::printf("max gain drift      %.3f dB/s (10 s samples)\n", driftMax);
    std::printf("band   fc(Hz)  dev(dB)  applied(dB)\n");
    for (int k = 0; k < kBands; ++k) {
        const double fc = sa::ThirdOctaveAnalyzer::bandCenter(k);
        if (fc < 60 || fc > 16000) continue;
        std::printf("%6.0f  %7.1f  %+6.1f   %+6.1f\n", fc, fc, dev[k], b[k] - a[k]);
    }
    return 0;
}
