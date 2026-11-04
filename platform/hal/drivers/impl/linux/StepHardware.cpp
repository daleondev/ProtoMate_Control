#include "StepHardware.hpp"
namespace hal::util
{
    LinuxStepHardware::~LinuxStepHardware() { stopService(); }
    void LinuxStepHardware::beginService(StepGenerator& generator)
    {
        simulation::client::Transaction guard{};
        simulation::client::setStepCallback([&generator] { generator.service(); });
    }
    void LinuxStepHardware::stopService() noexcept
    {
        simulation::client::Transaction guard{};
        simulation::client::setStepCallback({});
    }
    auto LinuxStepHardware::reset() noexcept -> bool
    {
        m_valid = {};
        return simulation::client::command(simulation::Operation::Reset).status == 0;
    }
    auto LinuxStepHardware::arm(std::size_t axis, std::uint32_t first, std::uint32_t count) noexcept -> bool
    {
        return simulation::client::command(simulation::Operation::Arm, { axis, first, count }).status == 0;
    }
    auto LinuxStepHardware::start(std::uint32_t tick) noexcept -> void
    {
        simulation::client::command(simulation::Operation::Start, { tick });
    }
    auto LinuxStepHardware::guard(std::uint32_t tick) noexcept -> void
    {
        simulation::client::command(simulation::Operation::Guard, { tick });
    }
    auto LinuxStepHardware::completionWatch(bool enabled) noexcept -> void
    {
        simulation::client::command(simulation::Operation::Watch, { enabled });
    }
    auto LinuxStepHardware::sample() noexcept -> StepSample { return simulation::client::sample(); }
    auto LinuxStepHardware::acknowledge(std::size_t axis) noexcept -> void
    {
        simulation::client::command(simulation::Operation::Acknowledge, { axis });
    }
    auto LinuxStepHardware::finishAxis(std::size_t axis) noexcept -> void
    {
        simulation::client::command(simulation::Operation::FinishAxis, { axis });
    }
    auto LinuxStepHardware::stopAxis(std::size_t axis) noexcept -> StepSample
    {
        auto value{ sample() };
        simulation::client::command(simulation::Operation::StopAxis, { axis });
        return value;
    }
    auto LinuxStepHardware::stop() noexcept -> StepSample
    {
        auto value{ sample() };
        simulation::client::command(simulation::Operation::Stop);
        return value;
    }
    auto LinuxStepHardware::publish() noexcept -> void
    {
        auto* shared{ simulation::client::memory() };
        if (!shared)
            return;
        for (unsigned axis{}; axis < 3; ++axis)
            for (unsigned bank{}; bank < 2; ++bank) {
                if (m_valid[axis][bank] && memory[axis][bank] == m_published[axis][bank])
                    continue;
                std::array<std::byte, sizeof(StepBuffer)> encoded{};
                for (unsigned i{}; i < STEP_BUFFER_EDGES; ++i)
                    simulation::put(encoded, i * 4, memory[axis][bank][i], 4);
                if (simulation::client::publishBank(axis, bank, encoded)) {
                    m_published[axis][bank] = memory[axis][bank];
                    m_valid[axis][bank] = true;
                }
            }
    }

}
