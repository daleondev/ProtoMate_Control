#include "Profile.hpp"
#include <cmath>
#include <stdexcept>
namespace simulation
{
    namespace
    {
        constexpr double MAX_OPERATE_DEFLECTION_DEG{ 15 };
    }
    void Profile::validate() const
    {
        for (double value : geometry_mm) {
            if (!std::isfinite(value) || value <= 0) {
                throw std::invalid_argument{ "invalid simulation geometry" };
            }
        }
        for (double value : ratio) {
            if (!std::isfinite(value) || value <= 0) {
                throw std::invalid_argument{ "invalid simulation gearing" };
            }
        }
        for (auto d : direction) {
            if (d != ui::motion::AxisDirection::SameAsMotor &&
                d != ui::motion::AxisDirection::OppositeToMotor) {
                throw std::invalid_argument{ "invalid simulation axis direction" };
            }
        }
        for (double value : shaft_reference) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument{ "invalid shaft reference" };
            }
        }
        for (double value : axis_reference) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument{ "invalid axis reference" };
            }
        }
        for (unsigned i{}; i < 3; ++i) {
            if (!std::isfinite(sensor_operate_deflection_deg[i]) ||
                !std::isfinite(sensor_release_clearance_mm[i]) || sensor_release_clearance_mm[i] <= 0 ||
                sensor_release_clearance_mm[i] > 1 || sensor_operate_deflection_deg[i] <= 0 ||
                sensor_operate_deflection_deg[i] > MAX_OPERATE_DEFLECTION_DEG) {
                throw std::invalid_argument{ "invalid switch lever travel" };
            }
        }
        if (!std::isfinite(z_mm_per_revolution) || z_mm_per_revolution <= 0 || dm542_microsteps == 0) {
            throw std::invalid_argument{ "invalid simulation profile" };
        }
    }
}
