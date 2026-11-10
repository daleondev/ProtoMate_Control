#pragma once

#include "Robot.hpp"
#include <concepts>
#include <functional>
#include <optional>

namespace ui::motion
{
    // A joint-unit facade over the one application-owned client/planning worker.
    // It owns no hardware or action lifetime; the client retains submitted jobs.
    template<class Position>
    class Axis final
    {
        static_assert(std::same_as<Position, pnm::units::Angle> ||
                      std::same_as<Position, pnm::units::Distance>);

      public:
        using Velocity = decltype(Position{} / pnm::units::Time{});
        using Acceleration = decltype(Velocity{} / pnm::units::Time{});
        using Jerk = decltype(Acceleration{} / pnm::units::Time{});
        struct Dynamics
        {
            Velocity speed{};
            Acceleration acceleration{}, deceleration{};
            Jerk jerk{};
        };
        std::uint64_t moveRel(Position distance, Dynamics dynamics = {}, pnm::units::Time timeout = {})
        {
            return move(distance, dynamics, timeout, true);
        }
        std::uint64_t moveAbs(Position position, Dynamics dynamics = {}, pnm::units::Time timeout = {})
        {
            return move(position, dynamics, timeout, false);
        }
        std::uint64_t home()
        {
            auto r{ request() };
            r.kind = Robot::ExecutionRequest::Kind::Home;
            return m_robot.get().submit(r);
        }
        // nullopt is continuous until stop, lease expiry or the soft boundary.
        std::uint64_t jog(Velocity speed,
                          std::optional<pnm::units::Time> duration = pnm::units::Time::create(1))
        {
            auto r{ request() };
            r.kind = Robot::ExecutionRequest::Kind::Jog;
            r.speed = speed.get();
            r.continuous = !duration;
            r.duration = duration ? duration->get() : 0;
            return m_robot.get().submit(r);
        }
        void stop() { m_robot.get().stopAxis(m_index); }

      private:
        friend class Robot;
        Axis(Robot& robot, unsigned index)
          : m_robot{ robot }
          , m_index{ index }
        {
        }
        Robot::ExecutionRequest request() const
        {
            Robot::ExecutionRequest r{};
            r.family = ::motion::Family::Axis;
            r.axes = std::uint8_t(1U << m_index);
            return r;
        }
        std::uint64_t move(Position position, Dynamics d, pnm::units::Time timeout, bool relative)
        {
            auto r{ request() };
            r.relative = relative;
            r.distance = position.get();
            r.speed = d.speed.get();
            r.acceleration = d.acceleration.get();
            r.deceleration = d.deceleration.get();
            r.jerk = d.jerk.get();
            r.timeout = timeout.get();
            return m_robot.get().submit(r);
        }
        std::reference_wrapper<Robot> m_robot;
        unsigned m_index;
    };
    using RotaryAxis = Axis<pnm::units::Angle>;
    using LinearAxis = Axis<pnm::units::Distance>;
}
