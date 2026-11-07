#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/visualizer_cli.h"

namespace {

std::vector<double> tone(double leftHz, double rightHz, int frames, double& t) {
    std::vector<double> out(frames * 2);
    for (int i = 0; i < frames; ++i, t += 1.0 / 48000) {
        out[2 * i] = std::trunc(12000 * std::sin(2 * M_PI * leftHz * t));
        out[2 * i + 1] = std::trunc(12000 * std::sin(2 * M_PI * rightHz * t));
    }
    return out;
}

int peak(const std::vector<int>& bars, int from, int to) {
    int best = from;
    for (int i = from; i < to; ++i)
        if (bars[i] > bars[best]) best = i;
    return best;
}

}

int main() {
    const float cavaCutOffs[] = {46.88f, 52.73f, 64.45f, 76.17f, 93.75f, 117.19f, 140.62f, 152.34f, 187.50f,
                                 210.94f, 246.09f, 292.97f, 339.84f, 398.44f, 468.75f, 550.78f, 632.81f, 750.00f,
                                 878.91f, 1019.53f, 1195.31f, 1406.25f, 1640.62f, 1921.88f, 2250.00f, 2636.72f, 3093.75f,
                                 3621.09f, 4242.19f, 4980.47f, 5835.94f, 6832.03f, 8003.91f};
    sa::CavaCore core;
    std::string error;
    assert(core.init(32, 48000, true, 0.77, 50, 8000, error));
    assert(core.inputSize() == 16384);
    for (int n = 0; n <= 32; ++n) assert(std::fabs(core.cutOffFrequencies()[n] - cavaCutOffs[n]) < 0.01f);
    assert(!core.init(32, 48000, true, 0.77, 50, 30000, error));
    assert(!core.init(5000, 48000, true, 0.77, 50, 8000, error));

    sa::Visualizer vis;
    sa::VisualizerOptions o;
    o.bars = 32;
    assert(vis.init(o, error));
    std::vector<int> bars;
    double t = 0;
    for (int f = 0; f < 120; ++f) {
        std::vector<double> in = tone(100, 2000, 800, t);
        bars = vis.execute(in.data(), static_cast<int>(in.size()));
    }
    for (int v : bars) assert(v >= 0 && v <= 1000);
    const int left = peak(bars, 0, 16), right = peak(bars, 16, 32);
    assert(bars[left] > 300 && bars[right] > 300);
    assert(left > 8 && right > 24);

    o.stereo = false;
    o.reverse = true;
    sa::Visualizer mono;
    assert(mono.init(o, error));
    t = 0;
    for (int f = 0; f < 120; ++f) {
        std::vector<double> in = tone(100, 100, 800, t);
        bars = mono.execute(in.data(), static_cast<int>(in.size()));
    }
    assert(peak(bars, 0, 32) > 24);

    std::vector<double> silence(1600, 0.0);
    for (int f = 0; f < 300; ++f) bars = mono.execute(silence.data(), static_cast<int>(silence.size()));
    for (int v : bars) assert(v == 0);

    o = {};
    o.bars = 31;
    assert(!vis.init(o, error));
    o.bars = 32;
    o.sensitivity = 0;
    assert(!vis.init(o, error));

    sa::VisualizerConfig cfg;
    assert(sa::parseVisualizerArgs({"--bars", "48", "--framerate=30", "--channels", "mono", "--mono-option", "left",
                                    "--eq", "1,0.5,2", "--monstercat", "1.5", "--autosens", "0", "--source", "foo.monitor"},
                                   cfg, error));
    assert(cfg.options.bars == 48 && cfg.options.framerate == 30 && !cfg.options.stereo && !cfg.options.autosens);
    assert(cfg.options.monoOption == sa::MonoOption::Left && cfg.options.eq.size() == 3 && cfg.options.eq[1] == 0.5);
    assert(cfg.options.monstercat == 1.5 && cfg.source == "foo.monitor");
    assert(!sa::parseVisualizerArgs({"--bars"}, cfg, error));
    assert(!sa::parseVisualizerArgs({"--bars", "x"}, cfg, error));
    assert(!sa::parseVisualizerArgs({"--channels", "quad"}, cfg, error));
    assert(!sa::parseVisualizerArgs({"--eq", "1,,2"}, cfg, error));
    assert(!sa::parseVisualizerArgs({"--colors", "1"}, cfg, error));

    sa::VisualizerConfig eqCfg;
    assert(sa::parseVisualizerArgs({"--bars", "4", "--channels", "mono", "--eq", "0,1"}, eqCfg, error));
    sa::Visualizer eqVis;
    assert(eqVis.init(eqCfg.options, error));
    t = 0;
    for (int f = 0; f < 60; ++f) {
        std::vector<double> in = tone(70, 70, 800, t);
        bars = eqVis.execute(in.data(), static_cast<int>(in.size()));
    }
    assert(bars[0] == 0 && bars[1] == 0);

    std::string line;
    sa::formatFrame({0, 1000, 5, 120, 999, 1200}, 1000, line);
    assert(line == "[0,1,0.005,0.12,0.999,1]\n");

    std::printf("visualizer ok\n");
    return 0;
}
