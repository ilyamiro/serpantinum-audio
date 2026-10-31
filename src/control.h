#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "dsp/params.h"

namespace sa {

inline const char* dynamicsName(Dynamics d) {
    switch (d) {
        case Dynamics::Balanced: return "balanced";
        case Dynamics::Night: return "night";
        default: return "off";
    }
}

inline bool parseDynamics(const std::string& s, Dynamics& out) {
    if (s == "off") out = Dynamics::Off;
    else if (s == "balanced") out = Dynamics::Balanced;
    else if (s == "night") out = Dynamics::Night;
    else return false;
    return true;
}

inline bool parseFloat(const std::string& s, float& out) {
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

inline bool parseBool(const std::string& s, bool& out) {
    if (s == "1" || s == "true") out = true;
    else if (s == "0" || s == "false") out = false;
    else return false;
    return true;
}

inline std::string serialize(const Params& p) {
    std::ostringstream o;
    o << "set";
    for (float g : p.gains) o << ' ' << g;
    o << ' ' << p.balance << ' ' << (p.mono ? 1 : 0) << ' ' << (p.swap ? 1 : 0) << ' ' << dynamicsName(p.dynamics)
      << ' ' << (p.bypass ? 1 : 0) << ' ' << (p.dynamicEq ? 1 : 0);
    return o.str();
}

inline bool applyCommand(Params& p, const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    std::vector<std::string> args;
    for (std::string a; in >> a;) args.push_back(a);
    Params next = p;

    if (cmd == "set") {
        if (args.size() != kEqBands + 5 && args.size() != kEqBands + 6) return false;
        for (int i = 0; i < kEqBands; ++i)
            if (!parseFloat(args[i], next.gains[i])) return false;
        if (!parseFloat(args[kEqBands], next.balance)) return false;
        if (!parseBool(args[kEqBands + 1], next.mono)) return false;
        if (!parseBool(args[kEqBands + 2], next.swap)) return false;
        if (!parseDynamics(args[kEqBands + 3], next.dynamics)) return false;
        if (!parseBool(args[kEqBands + 4], next.bypass)) return false;
        if (args.size() == kEqBands + 6 && !parseBool(args[kEqBands + 5], next.dynamicEq)) return false;
    } else if (cmd == "eq") {
        if (args.size() != kEqBands) return false;
        for (int i = 0; i < kEqBands; ++i)
            if (!parseFloat(args[i], next.gains[i])) return false;
    } else if (cmd == "band") {
        float idx, gain;
        if (args.size() != 2 || !parseFloat(args[0], idx) || !parseFloat(args[1], gain)) return false;
        const int i = static_cast<int>(idx);
        if (i < 1 || i > kEqBands) return false;
        next.gains[i - 1] = gain;
    } else if (cmd == "balance") {
        if (args.size() != 1 || !parseFloat(args[0], next.balance)) return false;
    } else if (cmd == "mono") {
        if (args.size() != 1 || !parseBool(args[0], next.mono)) return false;
    } else if (cmd == "swap") {
        if (args.size() != 1 || !parseBool(args[0], next.swap)) return false;
    } else if (cmd == "dyneq") {
        if (args.size() != 1 || !parseBool(args[0], next.dynamicEq)) return false;
    } else if (cmd == "bypass") {
        if (args.size() != 1 || !parseBool(args[0], next.bypass)) return false;
    } else if (cmd == "dynamics") {
        if (args.size() != 1 || !parseDynamics(args[0], next.dynamics)) return false;
    } else {
        return false;
    }

    if (next.balance < -1.0f) next.balance = -1.0f;
    if (next.balance > 1.0f) next.balance = 1.0f;
    p = next;
    return true;
}

}
