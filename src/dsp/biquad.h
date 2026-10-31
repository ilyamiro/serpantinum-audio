#pragma once

#include <cmath>

namespace sa {

struct BiquadCoeffs {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static BiquadCoeffs bandpass(double rate, double freq, double q) {
        const double w0 = 2.0 * M_PI * freq / rate;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double cw = std::cos(w0);
        const double a0 = 1.0 + alpha;
        BiquadCoeffs c;
        c.b0 = alpha / a0;
        c.b1 = 0.0;
        c.b2 = -alpha / a0;
        c.a1 = (-2.0 * cw) / a0;
        c.a2 = (1.0 - alpha) / a0;
        return c;
    }

    static BiquadCoeffs highShelf(double rate, double freq, double q, double gainDb) {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * M_PI * freq / rate;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double cw = std::cos(w0);
        const double sq = 2.0 * std::sqrt(a) * alpha;
        const double a0 = (a + 1.0) - (a - 1.0) * cw + sq;
        BiquadCoeffs c;
        c.b0 = a * ((a + 1.0) + (a - 1.0) * cw + sq) / a0;
        c.b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cw) / a0;
        c.b2 = a * ((a + 1.0) + (a - 1.0) * cw - sq) / a0;
        c.a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cw) / a0;
        c.a2 = ((a + 1.0) - (a - 1.0) * cw - sq) / a0;
        return c;
    }

    static BiquadCoeffs highpass(double rate, double freq, double q) {
        const double w0 = 2.0 * M_PI * freq / rate;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double cw = std::cos(w0);
        const double a0 = 1.0 + alpha;
        BiquadCoeffs c;
        c.b0 = (1.0 + cw) / 2.0 / a0;
        c.b1 = -(1.0 + cw) / a0;
        c.b2 = (1.0 + cw) / 2.0 / a0;
        c.a1 = (-2.0 * cw) / a0;
        c.a2 = (1.0 - alpha) / a0;
        return c;
    }

    double magnitudeDb(double rate, double freq) const {
        const double w = 2.0 * M_PI * freq / rate;
        const double c1 = std::cos(w), s1 = std::sin(w);
        const double c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
        const double nr = b0 + b1 * c1 + b2 * c2;
        const double ni = -(b1 * s1 + b2 * s2);
        const double dr = 1.0 + a1 * c1 + a2 * c2;
        const double di = -(a1 * s1 + a2 * s2);
        return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di) + 1e-30);
    }

    static BiquadCoeffs peaking(double rate, double freq, double q, double gainDb) {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * M_PI * freq / rate;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double cw = std::cos(w0);
        const double a0 = 1.0 + alpha / a;
        BiquadCoeffs c;
        c.b0 = (1.0 + alpha * a) / a0;
        c.b1 = (-2.0 * cw) / a0;
        c.b2 = (1.0 - alpha * a) / a0;
        c.a1 = (-2.0 * cw) / a0;
        c.a2 = (1.0 - alpha / a) / a0;
        return c;
    }
};

class Biquad {
public:
    void setCoeffs(const BiquadCoeffs& c) { c_ = c; }
    void reset() { z1_ = z2_ = 0.0; }

    float tick(float x) {
        const double in = x;
        const double out = c_.b0 * in + z1_;
        z1_ = c_.b1 * in - c_.a1 * out + z2_;
        z2_ = c_.b2 * in - c_.a2 * out;
        if (std::fabs(z1_) < 1e-30) z1_ = 0.0;
        if (std::fabs(z2_) < 1e-30) z2_ = 0.0;
        return static_cast<float>(out);
    }

private:
    BiquadCoeffs c_;
    double z1_ = 0.0, z2_ = 0.0;
};

}
