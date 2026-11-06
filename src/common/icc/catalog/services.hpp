#pragma once

#include "common/diagnostics/Memory.hpp"
#include "common/icc/transport/ITransport.hpp"
#include "common/motion/Motor.hpp"
#include "common/motion/Trajectory.hpp"

#include <cstdint>
#include <pneumo/messaging.hpp>

namespace icc
{
    enum class ServiceProvider : std::uint8_t
    {
        IO,
        UI
    };

    // Shared application contracts. Both bridges register this catalog.
    struct Services
    {
        // Versioned, explicitly serialized trajectory capabilities/admission.
        [[= ServiceProvider::IO]] pnm::msg::Service<motion::trajectory::Packet, motion::trajectory::Packet>
          io_trajectory;
        [[ = ServiceProvider::IO, = Lane::Urgent ]]
        pnm::msg::Service<motion::trajectory::Packet, motion::trajectory::Packet> io_trajectory_control;

        [[= ServiceProvider::IO]] pnm::msg::Service<diagnostics::MemoryQuery, diagnostics::MemorySnapshot>
          io_memory;

        [[= ServiceProvider::IO]] pnm::msg::Service<motion::MotorRequest, motion::MotorResponse> io_motor;

        [[ = ServiceProvider::IO,
           = Lane::Urgent ]] pnm::msg::Service<motion::MotorRequest, motion::MotorResponse>
          io_motor_stop;
    };
}
