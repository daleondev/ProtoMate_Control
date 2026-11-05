#pragma once
#include "common/motion/Profile.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace motion_bench
{
    // Monotone, positive braking profile. Integer-grid checks use the actual
    // Profile evaluator; Newton is only a predictor and never the acceptance test.
    class BrakeCrossing
    {
      public:
        explicit BrakeCrossing(const motion::Profile& profile)
          : profile(profile)
          , phases(profile.description())
        {
        }
        std::uint32_t next(double position)
        {
            while (phase + 1 < phases.size && position > phases.phases[phase + 1].position) {
                phaseStart += phases.phases[phase].duration;
                ++phase;
            }
            const auto& p = phases.phases[phase];
            double low = phaseStart, high = phaseStart + p.duration;
            double time = std::clamp(hint, low, high);
            const auto crossed = [&](std::uint32_t tick) {
                return profile.at(tick * 1e-7).position >= position;
            };
            for (unsigned iteration = 0; iteration < 8; ++iteration) {
                auto tick = std::uint32_t(std::ceil(time * 1e7));
                if (crossed(tick) && (tick == 0 || !crossed(tick - 1))) {
                    auto velocity = profile.at(tick * 1e-7).velocity;
                    hint = tick * 1e-7 + 1 / std::max(velocity, 1e-12);
                    return tick;
                }
                const double t = time - phaseStart;
                const double value =
                  p.position + t * (p.velocity + t * (p.acceleration * .5 + t * p.jerk / 6));
                const double slope = p.velocity + t * (p.acceleration + t * p.jerk * .5);
                if (value < position)
                    low = time;
                else
                    high = time;
                const double candidate = time - (value - position) / slope;
                time = std::isfinite(candidate) && candidate > low && candidate < high
                         ? candidate
                         : low + (high - low) * .5;
            }
            ++fallbacks;
            std::uint32_t a = std::uint32_t(std::floor(phaseStart * 1e7));
            std::uint32_t b = std::uint32_t(std::ceil((phaseStart + p.duration) * 1e7));
            while (a < b) {
                auto middle = a + (b - a) / 2;
                if (crossed(middle))
                    b = middle;
                else
                    a = middle + 1;
            }
            hint = a * 1e-7;
            return a;
        }
        unsigned fallbacks{};

      private:
        const motion::Profile& profile;
        motion::Profile::Description phases;
        unsigned phase{};
        double phaseStart{}, hint{};
    };
}
