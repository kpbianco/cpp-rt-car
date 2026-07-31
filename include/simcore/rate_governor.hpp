#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>

// Simple PI based rate governor. Controls a scale factor so that
// compute_ms stays within the frame budget. Includes hysteresis and clamping
// and provides rung callbacks for staged degradations.
class RateGovernor {
public:
    using RungCallback = std::function<void(int)>;

    RateGovernor(double kp = 0.1, double ki = 0.01, double floor = 0.5,
                 double ceiling = 1.0)
        : kp_(kp), ki_(ki), floor_(floor), ceiling_(ceiling) {}

    void setCallback(RungCallback cb) { rungCb_ = std::move(cb); }

    // Update with the last frame's compute time and budget information.
    // Returns the new scale factor recommendation.
    double update(double compute_ms, double frame_ms, double target_util,
                  double hysteresis) {
        if (frame_ms <= std::numeric_limits<double>::epsilon())
            return scale_;

        const double budget = std::max(frame_ms, std::numeric_limits<double>::epsilon());
        const double ratio = compute_ms / budget;

        const double target = std::clamp(target_util, 0.0, 1.0);
        const double hyst = std::max(hysteresis, 1e-6);
        const double error = ratio - target;

        if (std::abs(error) >= hyst) {
            integral_ += error;
            integral_ = std::clamp(integral_, -100.0, 100.0);
            const double delta = kp_ * error + ki_ * integral_;
            scale_ = std::clamp(scale_ - delta, floor_, ceiling_);
        }

        int targetRung = 0;
        if (ratio >= target + 3.0 * hyst)
            targetRung = 3;
        else if (ratio >= target + 2.0 * hyst)
            targetRung = 2;
        else if (ratio >= target + hyst)
            targetRung = 1;

        if (targetRung > rung_) {
            for (std::size_t index = 0; index < rungCounts_.size(); ++index) {
                const int rung = static_cast<int>(index) + 1;
                if (rung <= rung_ || rung > targetRung)
                    continue;
                rungCounts_[index]++;
                if (rungCb_)
                    rungCb_(rung);
            }
            rung_ = targetRung;
        }

        return scale_;
    }

    double scale() const { return scale_; }
    int rung() const { return rung_; }
    std::uint64_t rungCount(int rung) const {
        if (rung < 1 || rung > 3)
            return 0;
        return rungCounts_[static_cast<std::size_t>(rung - 1)];
    }

private:
    double kp_;
    double ki_;
    double floor_;
    double ceiling_;
    double integral_ = 0.0;
    double scale_ = 1.0;
    int rung_ = 0;
    std::array<std::uint64_t, 3> rungCounts_{};
    RungCallback rungCb_;
};
