#pragma once
#include <algorithm>
#include <cmath>
#include <functional>

// Simple PI based rate governor. Controls a scale factor so that
// compute_ms stays within frame_ms. Includes hysteresis and clamping.
class RateGovernor {
public:
    using RungCallback = std::function<void(int)>;

    RateGovernor(double frame_ms, double kp=0.1, double ki=0.01,
                 double floor=0.5, double ceiling=1.0, double hysteresis=0.05,
                 RungCallback rungCb = {})
        : frame_ms_(frame_ms), kp_(kp), ki_(ki), floor_(floor),
          ceiling_(ceiling), hyst_(hysteresis), rungCb_(std::move(rungCb)) {}

    // Update with the last frame's compute time and parameters. Returns new
    // scale factor.
    double update(double compute_ms, double frame_ms, double target_util,
                  double hysteresis) {
        frame_ms_ = frame_ms;
        target_ = target_util;
        hyst_ = hysteresis;

        double ratio = compute_ms / (frame_ms_ * scale_);
        double error = ratio - target_;
        if (std::abs(error) >= hyst_) {
            integral_ += error;
            double delta = kp_ * error + ki_ * integral_;
            scale_ = std::clamp(scale_ - delta, floor_, ceiling_);
        }

        const double ratioRef = compute_ms / frame_ms_;
        if (rung_ < 1 && ratioRef >= t1_) {
            rung_ = 1;
            if (rungCb_)
                rungCb_(1);
        }
        if (rung_ < 2 && ratioRef >= t2_) {
            rung_ = 2;
            if (rungCb_)
                rungCb_(2);
        }
        if (rung_ < 3 && ratioRef >= t3_) {
            rung_ = 3;
            if (rungCb_)
                rungCb_(3);
        }

        return scale_;
    }

    // Legacy update using stored parameters.
    double update(double compute_ms) {
        return update(compute_ms, frame_ms_, target_, hyst_);
    }

    double scale() const { return scale_; }
    int rung() const { return rung_; }

private:
    double frame_ms_;
    double kp_;
    double ki_;
    double floor_;
    double ceiling_;
    double hyst_;
    double target_ = 1.0;
    double integral_ = 0.0;
    double scale_ = 1.0;
    int rung_ = 0;
    const double t1_ = 1.1;
    const double t2_ = 1.3;
    const double t3_ = 1.5;
    RungCallback rungCb_;
};

