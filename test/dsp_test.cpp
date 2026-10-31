#include <cmath>
#include <array>
#include <cstdio>
#include <vector>

#include "../src/dsp/chain.h"

namespace {

int failures = 0;

void check(bool ok, const char* name, double got, double want) {
    std::printf("%s %-34s got %.4f want %.4f\n", ok ? "ok  " : "FAIL", name, got, want);
    if (!ok) ++failures;
}

constexpr double kRate = 48000.0;

std::vector<float> tone(double freq, double amp, size_t frames, double l = 1.0, double r = 1.0) {
    std::vector<float> v(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const double s = amp * std::sin(2.0 * M_PI * freq * i / kRate);
        v[2 * i] = static_cast<float>(s * l);
        v[2 * i + 1] = static_cast<float>(s * r);
    }
    return v;
}

double rmsDb(const std::vector<float>& v, size_t from, int ch) {
    double sum = 0;
    size_t n = 0;
    for (size_t i = from; i < v.size() / 2; ++i, ++n) sum += v[2 * i + ch] * v[2 * i + ch];
    return 10.0 * std::log10(sum / n + 1e-20);
}

sa::Chain makeChain(const sa::Params& p) {
    sa::Chain c;
    c.setSampleRate(kRate);
    c.setParams(p);
    return c;
}

void process(sa::Chain& c, std::vector<float>& v) {
    for (size_t off = 0; off < v.size() / 2; off += 480) {
        c.process(v.data() + off * 2, std::min<size_t>(480, v.size() / 2 - off));
    }
}

std::vector<float> noise(size_t frames, double lowpassHz, double lowMix, unsigned seed) {
    std::vector<float> v(frames * 2);
    unsigned state = seed;
    auto white = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / 8388608.0 - 1.0;
    };
    const double a = 1.0 - std::exp(-2.0 * M_PI * lowpassHz / kRate);
    double lp = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        const double w = white();
        lp += a * (w - lp);
        const double s = 0.05 * (lowMix * lp * 6.0 + (1.0 - lowMix) * w);
        v[2 * i] = v[2 * i + 1] = static_cast<float>(s);
    }
    return v;
}

}

int main() {
    {
        sa::Params p;
        auto c = makeChain(p);
        auto in = tone(1000, 0.5, 48000);
        auto out = in;
        process(c, out);
        check(in == out, "flat is bit-exact passthrough", in == out, 1);
    }
    for (int band : {0, 2, 5, 7, 9}) {
        sa::Params p;
        p.gains[band] = 6.0f;
        auto c = makeChain(p);
        auto in = tone(sa::Equalizer::kFreqs[band], 0.1, 96000);
        auto out = in;
        process(c, out);
        char name[48];
        std::snprintf(name, sizeof name, "band %d +6 dB at center", band);
        const double gain = rmsDb(out, 48000, 0) - rmsDb(in, 48000, 0);
        check(std::fabs(gain - 6.0) < 0.15, name, gain, 6.0);
    }
    {
        sa::Params p;
        p.gains[5] = 6.0f;
        auto c = makeChain(p);
        auto in = tone(100, 0.1, 96000);
        auto out = in;
        process(c, out);
        const double gain = rmsDb(out, 48000, 0) - rmsDb(in, 48000, 0);
        check(std::fabs(gain) < 1.0, "1 kHz boost leaves 100 Hz ~flat", gain, 0.0);
    }
    {
        sa::Params p;
        p.mono = true;
        auto c = makeChain(p);
        auto v = tone(500, 0.4, 9600, 1.0, 0.0);
        auto in = v;
        process(c, v);
        bool ok = true;
        for (size_t i = 2400; i < 9600; ++i)
            ok &= std::fabs(v[2 * i] - 0.5f * in[2 * i]) < 1e-5f && v[2 * i] == v[2 * i + 1];
        check(ok, "mono sums to both channels", ok, 1);
    }
    {
        sa::Params p;
        p.swap = true;
        auto c = makeChain(p);
        auto v = tone(500, 0.4, 9600, 1.0, 0.0);
        auto in = v;
        process(c, v);
        bool ok = true;
        for (size_t i = 0; i < 9600; ++i) ok &= v[2 * i] == in[2 * i + 1] && v[2 * i + 1] == in[2 * i];
        check(ok, "swap exchanges channels", ok, 1);
    }
    {
        sa::Params p;
        p.balance = 0.5f;
        auto c = makeChain(p);
        auto v = tone(500, 0.4, 48000);
        process(c, v);
        const double l = rmsDb(v, 24000, 0), r = rmsDb(v, 24000, 1);
        check(std::fabs((l - r) - 20.0 * std::log10(0.5)) < 0.05, "balance +0.5 attenuates left by half", l - r,
              20.0 * std::log10(0.5));
    }
    {
        sa::Params p;
        p.dynamics = sa::Dynamics::Balanced;
        auto c = makeChain(p);
        auto in = tone(1000, 0.5, 96000);
        auto out = in;
        process(c, out);
        const double levelDb = rmsDb(in, 48000, 0);
        const double want = -26.0 + (levelDb + 26.0) / 2.8 - levelDb;
        const double gain = rmsDb(out, 72000, 0) - rmsDb(in, 72000, 0);
        check(std::fabs(gain - want) < 0.4, "balanced compressor steady-state gain", gain, want);
    }
    {
        sa::Params p;
        p.dynamics = sa::Dynamics::Night;
        auto c = makeChain(p);
        auto in = tone(1000, 0.002, 96000);
        auto out = in;
        process(c, out);
        const double gain = rmsDb(out, 72000, 0) - rmsDb(in, 72000, 0);
        check(std::fabs(gain) < 0.1, "quiet signal below knee is untouched", gain, 0.0);
    }
    {
        sa::Params p;
        p.dynamics = sa::Dynamics::Balanced;
        auto c = makeChain(p);
        auto v = tone(220, 0.1, 48000);
        auto burst = tone(220, 3.0, 9600);
        for (size_t i = 0; i < burst.size(); ++i) v[24000 * 2 + i] = burst[i];
        process(c, v);
        float peak = 0;
        for (float s : v) peak = std::max(peak, std::fabs(s));
        const double ceiling = std::pow(10.0, -1.0 / 20.0);
        check(peak <= ceiling + 1e-6, "limiter never exceeds -1 dBFS", peak, ceiling);
    }
    {
        sa::Params p;
        p.gains[3] = 12.0f;
        p.bypass = true;
        auto c = makeChain(p);
        auto in = tone(250, 0.3, 4800);
        auto out = in;
        process(c, out);
        check(in == out, "bypass leaves audio untouched", in == out, 1);
    }
    {
        sa::Params p;
        p.dynamicEq = true;
        auto c = makeChain(p);
        std::vector<float> silence(48000 * 2 * 10, 0.0f);
        auto out = silence;
        process(c, out);
        bool clean = true;
        for (float s : out) clean &= s == 0.0f;
        check(clean, "dynamic eq leaves silence silent", clean, 1);
    }
    {
        sa::Params withManual;
        withManual.dynamicEq = true;
        withManual.gains[5] = 12.0f;
        sa::Params withoutManual;
        withoutManual.dynamicEq = true;
        auto a = makeChain(withManual);
        auto b = makeChain(withoutManual);
        auto in = noise(48000 * 6, 180.0, 0.9, 3);
        auto outA = in;
        auto outB = in;
        process(a, outA);
        process(b, outB);
        double worst = 0;
        for (size_t i = 48000 * 2 * 2; i < outA.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(outA[i] - outB[i])));
        check(worst < 1e-4, "manual eq is ignored while dynamic eq is on", worst, 0.0);
    }
    {
        sa::Params p;
        p.dynamicEq = true;
        auto c = makeChain(p);
        auto warm = noise(48000 * 20, 180.0, 0.9, 5);
        process(c, warm);
        p.dynamicEq = false;
        c.setParams(p);
        auto in = tone(500, 0.1, 96000);
        auto out = in;
        process(c, out);
        const double gain = rmsDb(out, 72000, 0) - rmsDb(in, 72000, 0);
        check(std::fabs(gain) < 0.05, "turning dynamic eq off returns to flat", gain, 0.0);
    }
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
