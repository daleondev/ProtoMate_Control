#pragma once
#include "Protocol.hpp"

namespace control
{
    // UI-facing endpoint. Synchronous methods are for the CLI/planning worker, not
    // the context runner. GUI coroutines can use requestAsync and the action endpoint.
    class ControllerClient final : public ControllerTypes
    {
      public:
        ControllerClient(pnm::msg::Bus& bus, pnm::coro::Context& context);
        ~ControllerClient();
        ControllerClient(const ControllerClient&) = delete;
        ControllerClient& operator=(const ControllerClient&) = delete;
        ControllerClient(ControllerClient&&) = delete;
        ControllerClient& operator=(ControllerClient&&) = delete;
        void enable();
        void disable();
        void stop(std::optional<MotorId> motor = std::nullopt);
        void reset();
        void initializeDrivers();
        void configureDriver(MotorId id, DriverConfiguration c);
        void configureAxis(MotorId id, Conversion c);
        Status status() const;
        PlanningSnapshot planningSnapshot() const;
        std::array<DriverStatus, 3> driverStatus() const;
        AxisConfig motorConfiguration(MotorId id) const;
        Conversion axisConversion(MotorId id) const;
        MotionDefaults defaults(MotorId id) const;
        void setDefaults(MotorId id, MotionDefaults d);
        AxisDefaults axisDefaults(MotorId id) const;
        void setAxisDefaults(MotorId id, const AxisDefaults& d);
        hal::step::PulseCount setVelocity(MotorId id, pnm::units::AngularVelocity speed);
        hal::step::PulseCount setAxisVelocity(MotorId id, const AxisSpeed& speed);
        Motion move(MotorId id, const Move& m);
        Motion moveAxis(MotorId id, const AxisMove& m);
        Motion reference(MotorId id,
                         pnm::units::AngularVelocity seek = control::DEFAULT_REFERENCE_SEEK,
                         pnm::units::AngularVelocity latch = control::DEFAULT_REFERENCE_LATCH,
                         pnm::units::Time timeout = control::DEFAULT_REFERENCE_TIMEOUT);
        Motion referenceAxis(MotorId id,
                             std::optional<AxisSpeed> seek = {},
                             std::optional<AxisSpeed> latch = {},
                             pnm::units::Time timeout = control::DEFAULT_REFERENCE_TIMEOUT);
        GroupMotion referenceAll(pnm::units::Time timeout = control::DEFAULT_GROUP_REFERENCE_TIMEOUT);
        GroupMotion coordinate(const Trajectory& t, pnm::units::Time timeout = 0_s);
        std::vector<Motion> motions();
        std::vector<GroupMotion> groupMotions();
        void cancel(MotionId id);
        pnm::coro::Task<protocol::Response> requestAsync(protocol::Request q);

      private:
        struct State;
        std::shared_ptr<State> m_state;
        protocol::Response request(const protocol::Request& q) const;
        Motion submit(const protocol::Request& q, bool group, pnm::units::Time duration = {});
    };
}
