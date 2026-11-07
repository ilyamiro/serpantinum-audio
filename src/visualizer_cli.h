#pragma once

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "dsp/visualizer.h"

namespace sa {

constexpr const char* kVisualizerUsage =
    "usage: serpantinum-audio visualizer [options]\n"
    "\n"
    "Captures the default output and prints one frame per line, a JSON array of bar heights from 0 to 1.\n"
    "A frame equal to the previous one is not printed, so silence ends with a single frame of zeros.\n"
    "\n"
    "  --bars N             number of bars, even in stereo (64)\n"
    "  --framerate N        frames per second (60)\n"
    "  --sensitivity N      sensitivity in percent, applied on top of autosens (100)\n"
    "  --autosens 0|1       adjust the sensitivity automatically to use the full range (1)\n"
    "  --noise-reduction N  0 to 100, higher is slower and smoother (77)\n"
    "  --lower-cutoff HZ    lowest frequency shown (50)\n"
    "  --higher-cutoff HZ   highest frequency shown (8000)\n"
    "  --channels MODE      stereo: left channel mirrored on the left, right on the right, bass in the\n"
    "                       centre; mono: one channel, bass first (stereo)\n"
    "  --mono-option MODE   average, left or right, for mono (average)\n"
    "  --reverse 0|1        show the frequencies the other way around (0)\n"
    "  --monstercat N       monstercat smoothing strength, 0 is off (0)\n"
    "  --waves 0|1          wave style monstercat smoothing, needs --monstercat (0)\n"
    "  --eq LIST            comma separated gains spread from the lowest to the highest bars (off)\n"
    "  --source NAME        auto (default output), auto_input (default input), a node name, or\n"
    "                       <sink>.monitor (auto)\n"
    "  --sample-rate N      capture rate in Hz (48000)\n";

struct VisualizerConfig {
    VisualizerOptions options;
    std::string source = "auto";
};

inline bool parseInt(const std::string& s, int& out) {
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || errno || v < -1000000000L || v > 1000000000L) return false;
    out = static_cast<int>(v);
    return true;
}

inline bool parseDouble(const std::string& s, double& out) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

inline bool parseSwitch(const std::string& s, bool& out) {
    if (s == "1" || s == "true") out = true;
    else if (s == "0" || s == "false") out = false;
    else return false;
    return true;
}

inline bool parseList(const std::string& s, std::vector<double>& out) {
    out.clear();
    size_t start = 0;
    while (true) {
        const size_t comma = s.find(',', start);
        double v;
        if (!parseDouble(s.substr(start, comma == std::string::npos ? std::string::npos : comma - start), v)) return false;
        out.push_back(v);
        if (comma == std::string::npos) return true;
        start = comma + 1;
    }
}

inline bool parseVisualizerArgs(const std::vector<std::string>& args, VisualizerConfig& cfg, std::string& error) {
    VisualizerOptions& o = cfg.options;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string name = args[i];
        std::string value;
        const size_t eq = name.find('=');
        if (eq != std::string::npos) {
            value = name.substr(eq + 1);
            name.resize(eq);
        } else if (i + 1 < args.size()) {
            value = args[++i];
        } else {
            error = "missing value for " + name;
            return false;
        }

        bool ok;
        if (name == "--bars") ok = parseInt(value, o.bars);
        else if (name == "--framerate") ok = parseInt(value, o.framerate);
        else if (name == "--sensitivity") ok = parseInt(value, o.sensitivity);
        else if (name == "--autosens") ok = parseSwitch(value, o.autosens);
        else if (name == "--noise-reduction") ok = parseInt(value, o.noiseReduction);
        else if (name == "--lower-cutoff") ok = parseInt(value, o.lowerCutoff);
        else if (name == "--higher-cutoff") ok = parseInt(value, o.higherCutoff);
        else if (name == "--reverse") ok = parseSwitch(value, o.reverse);
        else if (name == "--monstercat") ok = parseDouble(value, o.monstercat);
        else if (name == "--waves") ok = parseSwitch(value, o.waves);
        else if (name == "--eq") ok = parseList(value, o.eq);
        else if (name == "--sample-rate") ok = parseInt(value, o.sampleRate);
        else if (name == "--source") ok = !(cfg.source = value).empty();
        else if (name == "--channels") {
            ok = value == "stereo" || value == "mono";
            o.stereo = value == "stereo";
        } else if (name == "--mono-option") {
            ok = true;
            if (value == "average") o.monoOption = MonoOption::Average;
            else if (value == "left") o.monoOption = MonoOption::Left;
            else if (value == "right") o.monoOption = MonoOption::Right;
            else ok = false;
        } else {
            error = "unknown option " + name;
            return false;
        }
        if (!ok) {
            error = "invalid value for " + name + ": " + value;
            return false;
        }
    }
    return true;
}

inline void formatFrame(const std::vector<int>& bars, int range, std::string& out) {
    out.clear();
    out += '[';
    char buf[32];
    for (size_t i = 0; i < bars.size(); ++i) {
        if (i) out += ',';
        const int v = bars[i];
        if (v <= 0) {
            out += '0';
        } else if (v >= range) {
            out += '1';
        } else {
            std::snprintf(buf, sizeof buf, "%.6f", static_cast<double>(v) / range);
            std::string s = buf;
            while (s.back() == '0') s.pop_back();
            out += s;
        }
    }
    out += "]\n";
}

}
