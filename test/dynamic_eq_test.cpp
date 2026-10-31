#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "../src/dsp/chain.h"
#include "../src/dsp/fft.h"

namespace {

constexpr double kRate = 48000.0;
constexpr int kBands = sa::ThirdOctaveAnalyzer::kBands;
int failures = 0;

void check(bool ok, const char* name, double got, double want) {
    std::printf("%s %-52s got %8.3f want %8.3f\n", ok ? "ok  " : "FAIL", name, got, want);
    if (!ok) ++failures;
}

std::vector<float> synth(double seconds, const std::function<double(double)>& shapeDb, unsigned seed) {
    size_t n = 1;
    while (n < static_cast<size_t>(seconds * kRate)) n <<= 1;
    const size_t frames = static_cast<size_t>(seconds * kRate);
    std::vector<float> out(frames * 2);
    sa::Fft fft(n);
    for (int ch = 0; ch < 2; ++ch) {
        std::vector<float> re(n, 0.0f), im(n, 0.0f);
        unsigned state = seed * 2654435761u + static_cast<unsigned>(ch) * 40503u + 1u;
        auto uniform = [&state]() {
            state = state * 1664525u + 1013904223u;
            return static_cast<double>(state >> 8) / 16777216.0;
        };
        const double slopeExp = -(-sa::DynamicEq::kTargetSlopeDbPerOctave + 3.0103) / 6.0206;
        for (size_t k = 1; k < n / 2; ++k) {
            const double f = static_cast<double>(k) * kRate / static_cast<double>(n);
            const double amp = std::pow(f, slopeExp) * std::pow(10.0, shapeDb(f) / 20.0);
            const double phase = 2.0 * M_PI * uniform();
            re[k] = static_cast<float>(amp * std::cos(phase));
            im[k] = static_cast<float>(-amp * std::sin(phase));
            re[n - k] = re[k];
            im[n - k] = -im[k];
        }
        fft.forward(re.data(), im.data());
        double sum = 0.0;
        for (size_t i = 0; i < frames; ++i) sum += static_cast<double>(re[i]) * re[i];
        const double gain = std::pow(10.0, -23.0 / 20.0) / std::sqrt(sum / static_cast<double>(frames));
        for (size_t i = 0; i < frames; ++i) out[2 * i + ch] = static_cast<float>(re[i] * gain);
    }
    return out;
}

std::array<double, kBands> bandLevels(const std::vector<float>& v, double fromSeconds, double toSeconds) {
    sa::ThirdOctaveAnalyzer analyzer;
    analyzer.setSampleRate(kRate);
    std::array<double, kBands> acc{};
    int blocks = 0;
    const size_t from = static_cast<size_t>(fromSeconds * kRate);
    const size_t to = std::min(v.size() / 2, static_cast<size_t>(toSeconds * kRate));
    analyzer.push(v.data() + from * 2, to - from, [&](const sa::ThirdOctaveAnalyzer::Bands& b, float) {
        for (int k = 0; k < kBands; ++k) acc[k] += b[k];
        ++blocks;
    });
    std::array<double, kBands> db{};
    for (int k = 0; k < kBands; ++k) db[k] = 10.0 * std::log10(acc[k] / std::max(blocks, 1) + 1e-20);
    return db;
}

double deviationRms(const std::array<double, kBands>& levels) {
    std::array<double, kBands> dev{};
    double mean = 0.0, wsum = 0.0;
    for (int k = 0; k < kBands; ++k) {
        const double w = sa::DynamicEq::weightAt(sa::ThirdOctaveAnalyzer::bandCenter(k));
        dev[k] = levels[k] - sa::DynamicEq::kTargetSlopeDbPerOctave * sa::ThirdOctaveAnalyzer::bandOffsetOctaves(k);
        mean += w * dev[k];
        wsum += w;
    }
    mean /= wsum;
    double var = 0.0;
    for (int k = 0; k < kBands; ++k) {
        const double w = sa::DynamicEq::weightAt(sa::ThirdOctaveAnalyzer::bandCenter(k));
        var += w * (dev[k] - mean) * (dev[k] - mean);
    }
    return std::sqrt(var / wsum);
}

double idealResidualRms(const std::array<double, kBands>& levels) {
    std::array<double, kBands> dev{};
    double mean = 0.0, wsum = 0.0;
    for (int k = 0; k < kBands; ++k) {
        const double w = sa::DynamicEq::weightAt(sa::ThirdOctaveAnalyzer::bandCenter(k));
        dev[k] = levels[k] - sa::DynamicEq::kTargetSlopeDbPerOctave * sa::ThirdOctaveAnalyzer::bandOffsetOctaves(k);
        mean += w * dev[k];
        wsum += w;
    }
    mean /= wsum;
    std::array<double, kBands> corrected{};
    for (int k = 0; k < kBands; ++k) {
        const double w = sa::DynamicEq::weightAt(sa::ThirdOctaveAnalyzer::bandCenter(k));
        corrected[k] = levels[k] + sa::DynamicEq::shapeCorrection(static_cast<float>(dev[k] - mean)) * w;
    }
    return deviationRms(corrected);
}

double kWeightedDb(const std::array<double, kBands>& levels) {
    const sa::BiquadCoeffs shelf = sa::BiquadCoeffs::highShelf(kRate, 1681.97, 0.7072, 3.9998);
    const sa::BiquadCoeffs rlb = sa::BiquadCoeffs::highpass(kRate, 38.13, 0.5003);
    double sum = 0.0;
    for (int k = 0; k < kBands; ++k) {
        const double fc = sa::ThirdOctaveAnalyzer::bandCenter(k);
        sum += std::pow(10.0, (levels[k] + shelf.magnitudeDb(kRate, fc) + rlb.magnitudeDb(kRate, fc)) / 10.0);
    }
    return 10.0 * std::log10(sum);
}

double maxAbsDiff(const std::array<double, kBands>& a, const std::array<double, kBands>& b, double lo, double hi) {
    double worst = 0.0;
    for (int k = 0; k < kBands; ++k) {
        const double fc = sa::ThirdOctaveAnalyzer::bandCenter(k);
        if (fc >= lo && fc <= hi) worst = std::max(worst, std::fabs(a[k] - b[k]));
    }
    return worst;
}

std::vector<float> run(const std::vector<float>& in, std::vector<std::array<float, sa::DynamicEq::kBank>>* gainLog = nullptr) {
    sa::DynamicEq eq;
    eq.setSampleRate(kRate);
    eq.setEnabled(true);
    auto out = in;
    const size_t frames = in.size() / 2;
    size_t nextLog = 0;
    for (size_t off = 0; off < frames; off += 512) {
        eq.process(out.data() + off * 2, std::min<size_t>(512, frames - off));
        if (gainLog && off >= nextLog) {
            gainLog->push_back(eq.appliedGains());
            nextLog += static_cast<size_t>(kRate);
        }
    }
    return out;
}

double bassHeavy(double f) { return f < 100.0 ? 8.0 : (f < 400.0 ? 8.0 * (1.0 - std::log2(f / 100.0) / 2.0) : 0.0); }
double bright(double f) { return f > 3000.0 ? 8.0 : (f > 1500.0 ? 8.0 * std::log2(f / 1500.0) : 0.0); }
double balanced(double) { return 0.0; }

}

int main() {
    {
        const auto in = synth(40.0, balanced, 1);
        const auto out = run(in);
        const auto a = bandLevels(in, 20, 40), b = bandLevels(out, 20, 40);
        const double worst = maxAbsDiff(a, b, 100, 10000);
        check(worst < 1.2, "balanced music is left alone (max band change dB)", worst, 1.2);
        double lift = 0.0;
        int count = 0;
        for (int k = 0; k < kBands; ++k) {
            const double fc = sa::ThirdOctaveAnalyzer::bandCenter(k);
            if (fc >= 55.0 && fc <= 80.0) {
                lift += b[k] - a[k];
                ++count;
            }
        }
        lift /= count;
        check(lift > 0.4 && lift < sa::DynamicEq::kBassPreferenceDb + 0.5, "balanced music gets the small bass preference (dB)", lift, sa::DynamicEq::kBassPreferenceDb);
    }
    for (int variant = 0; variant < 2; ++variant) {
        const auto in = synth(45.0, variant ? bright : bassHeavy, 2 + variant);
        std::vector<std::array<float, sa::DynamicEq::kBank>> log;
        const auto out = run(in, &log);
        const auto a = bandLevels(in, 25, 45), b = bandLevels(out, 25, 45);
        const double before = deviationRms(a), after = deviationRms(b), ideal = idealResidualRms(a);
        char name[96];
        std::snprintf(name, sizeof name, "%s: deviation shrinks (rms dB)", variant ? "bright" : "bass-heavy");
        check(after < before - 0.3, name, after, before - 0.3);
        std::snprintf(name, sizeof name, "%s: realized curve tracks the ideal one (rms dB)", variant ? "bright" : "bass-heavy");
        check(after < ideal + 0.4, name, after, ideal + 0.4);

        const double dLoud = kWeightedDb(b) - kWeightedDb(a);
        std::snprintf(name, sizeof name, "%s: K-weighted loudness preserved (dB)", variant ? "bright" : "bass-heavy");
        check(std::fabs(dLoud) < 0.6, name, dLoud, 0.6);

        double maxCut = 0.0, maxBoost = 0.0;
        for (int k = 0; k < kBands; ++k) {
            const double fc = sa::ThirdOctaveAnalyzer::bandCenter(k);
            if (fc < 100 || fc > 10000) continue;
            const double d = b[k] - a[k];
            maxCut = std::min(maxCut, d);
            maxBoost = std::max(maxBoost, d);
        }
        std::snprintf(name, sizeof name, "%s: cut within limit (dB)", variant ? "bright" : "bass-heavy");
        check(maxCut > -(sa::DynamicEq::kCutLimitDb + 2.0), name, maxCut, -(sa::DynamicEq::kCutLimitDb + 2.0));
        std::snprintf(name, sizeof name, "%s: boost within limit (dB)", variant ? "bright" : "bass-heavy");
        check(maxBoost < sa::DynamicEq::kBoostLimitDb + 2.0, name, maxBoost, sa::DynamicEq::kBoostLimitDb + 2.0);

        double wobble = 0.0;
        for (size_t i = 30; i + 1 < log.size(); ++i)
            for (int j = 0; j < sa::DynamicEq::kBank; ++j) wobble = std::max(wobble, static_cast<double>(std::fabs(log[i + 1][j] - log[i][j])));
        std::snprintf(name, sizeof name, "%s: steady-state gain drift per second (dB)", variant ? "bright" : "bass-heavy");
        check(wobble < 0.3, name, wobble, 0.3);
    }
    {
        auto in = synth(30.0, bassHeavy, 5);
        const auto second = synth(30.0, bright, 6);
        in.insert(in.end(), second.begin(), second.end());
        const auto out = run(in);
        const auto a = bandLevels(in, 42, 60), b = bandLevels(out, 42, 60);
        const double before = deviationRms(a), after = deviationRms(b);
        check(after < before * 0.8, "track change: adapts to the new track within 12 s", after, before * 0.8);
    }
    {
        const auto in = synth(25.0, bassHeavy, 7);
        const auto out = run(in);
        const auto a = bandLevels(in, 10, 25), b = bandLevels(out, 10, 25);
        const double before = deviationRms(a), after = deviationRms(b);
        check(after < before * 0.9, "convergence: already improving 10 s after enabling", after, before * 0.9);
    }
    {
        std::vector<float> quiet(static_cast<size_t>(kRate) * 2 * 30);
        unsigned state = 9;
        for (auto& s : quiet) {
            state = state * 1664525u + 1013904223u;
            s = 3e-5f * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
        }
        sa::DynamicEq eq;
        eq.setSampleRate(kRate);
        eq.setEnabled(true);
        for (size_t off = 0; off < quiet.size() / 2; off += 512) eq.process(quiet.data() + off * 2, std::min<size_t>(512, quiet.size() / 2 - off));
        double worst = 0.0;
        for (float g : eq.appliedGains()) worst = std::max(worst, static_cast<double>(std::fabs(g)));
        check(worst == 0.0, "sub-gate noise never steers the correction", worst, 0.0);
    }
    {
        sa::Params p;
        p.dynamicEq = true;
        sa::Chain c;
        c.setSampleRate(kRate);
        c.setParams(p);
        auto in = synth(30.0, bassHeavy, 8);
        float peak = 0.0f;
        for (float& s : in) s *= 14.0f;
        auto out = in;
        for (size_t off = 0; off < out.size() / 2; off += 512) c.process(out.data() + off * 2, std::min<size_t>(512, out.size() / 2 - off));
        for (size_t i = 48000 * 2; i < out.size(); ++i) peak = std::max(peak, std::fabs(out[i]));
        const float ceiling = std::pow(10.0f, sa::Limiter::kSafetyThresholdDb / 20.0f);
        check(peak <= ceiling + 1e-4f && peak > ceiling - 0.05f, "safety limiter engages and holds -0.1 dBFS", peak, ceiling);
    }
    {
        const auto in = synth(20.0, bassHeavy, 10);
        sa::DynamicEq eq;
        eq.setSampleRate(kRate);
        eq.setEnabled(true);
        auto out = in;
        for (size_t off = 0; off < out.size() / 2; off += 512) eq.process(out.data() + off * 2, std::min<size_t>(512, out.size() / 2 - off));
        eq.setEnabled(false);
        std::vector<float> tail(static_cast<size_t>(kRate) * 2 * 4, 0.0f);
        for (size_t i = 0; i < tail.size() / 2; ++i) tail[2 * i] = tail[2 * i + 1] = 0.1f * std::sin(2.0f * static_cast<float>(M_PI) * 500.0f * static_cast<float>(i) / static_cast<float>(kRate));
        auto ref = tail;
        for (size_t off = 0; off < tail.size() / 2; off += 512) eq.process(tail.data() + off * 2, std::min<size_t>(512, tail.size() / 2 - off));
        double worst = 0.0;
        for (size_t i = static_cast<size_t>(kRate) * 2 * 3; i < tail.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(tail[i] - ref[i])));
        check(worst < 1e-4, "disabling returns to a clean passthrough", worst, 1e-4);
    }
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
