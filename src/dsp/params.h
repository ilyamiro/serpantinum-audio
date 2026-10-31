#pragma once

#include <array>

namespace sa {

constexpr int kEqBands = 10;

enum class Dynamics { Off, Balanced, Night };

struct Params {
    std::array<float, kEqBands> gains{};
    float balance = 0.0f;
    bool mono = false;
    bool swap = false;
    Dynamics dynamics = Dynamics::Off;
    bool bypass = false;
    bool dynamicEq = false;
};

}
