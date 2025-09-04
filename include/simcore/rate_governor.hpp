#pragma once
#include <algorithm>

// Simple PI based rate governor. Controls a scale factor so that
// compute_ms stays within frame_ms. Includes hysteresis and clamping.
class RateGovernor {
public:
    RateGovernor(double frame_ms, double kp=0.1, double ki=0.01,
                 double floor=0.5, double ceiling=1.0, double hysteresis=0.05)
        : frame_ms_(frame_ms), kp_(kp), ki_(ki),
          floor_(floor), ceiling_(ceiling), hyst_(hysteresis) {}

    // Update with the last frame's compute time. Returns new scale factor.
    double update(double compute_ms) {
        double ratio = compute_ms / (frame_ms_ * scale_);
        double error = ratio - 1.0;
        if (std::abs(error) < hyst_) return scale_;
        integral_ += error;
        double delta = kp_ * error + ki_ * integral_;
        scale_ = std::clamp(scale_ - delta, floor_, ceiling_);
        return scale_;
    }

    double scale() const { return scale_; }

private:
    double frame_ms_;
    double kp_;
    double ki_;
    double floor_;
    double ceiling_;
    double hyst_;
    double integral_ = 0.0;
    double scale_ = 1.0;
};

