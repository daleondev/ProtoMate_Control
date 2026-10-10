#pragma once
#include "Protocol.hpp"

namespace control
{
    // UI-facing endpoint. Synchronous methods are for the CLI/planning worker, not
    // the context runner. GUI coroutines can use requestAsync and the action endpoint.
    class ControllerClient final : public ControllerTypes
    {
      public:
        ControllerClient(pnm::msg::Bus&, pnm::coro::Context&);
        ~ControllerClient();
        ControllerClient(const ControllerClient&) = delete;
        ControllerClient& operator=(const ControllerClient&) = delete;
        void enable();
        void disable();
        void stop(std::optional<MotorId> motor = std::nullopt);
        void reset();
        void initializeDrivers();
        void configureDriver(MotorId, DriverConfiguration);
        void configureAxis(MotorId, Conversion);
        Status status() const;
        PlanningSnapshot planningSnapshot() const;
        std::array<DriverStatus, 3> driverStatus() const;
        AxisConfig motorConfiguration(MotorId) const;
        Conversion axisConversion(MotorId) const;
        MotionDefaults defaults(MotorId) const;
        void setDefaults(MotorId, MotionDefaults);
        AxisDefaults axisDefaults(MotorId) const;
        void setAxisDefaults(MotorId, const AxisDefaults&);
        hal::step::PulseCount setVelocity(MotorId, pnm::units::AngularVelocity);
        hal::step::PulseCount setAxisVelocity(MotorId, const AxisSpeed&);
        Motion move(MotorId, const Move&);
        Motion moveAxis(MotorId, const AxisMove&);
        Motion reference(MotorId,
                         pnm::units::AngularVelocity seek = 5_rpm,
                         pnm::units::AngularVelocity latch = 0.5_rpm,
                         pnm::units::Time timeout = 30_s);
        Motion referenceAxis(MotorId,
                             std::optional<AxisSpeed> seek = {},
                             std::optional<AxisSpeed> latch = {},
                             pnm::units::Time timeout = 30_s);
        GroupMotion referenceAll(pnm::units::Time timeout = 90_s);
        GroupMotion coordinate(const Trajectory&, pnm::units::Time timeout = 0_s);
        std::vector<Motion> motions();
        std::vector<GroupMotion> groupMotions();
        void cancel(MotionId);
        pnm::coro::Task<protocol::Response> requestAsync(protocol::Request request);

      private:
        struct State;
        std::shared_ptr<State> m_state;
        protocol::Response request(protocol::Request) const;
        Motion submit(protocol::Request, bool group, pnm::units::Time duration = {});
    };
}
