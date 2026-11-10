#include "StepHardware.hpp"
#include <bit>

namespace hal::util
{
    LinuxStepHardware::~LinuxStepHardware() { stopService(); }
    void LinuxStepHardware::beginService(StepGenerator& generator)
    {
        simulation::client::Transaction guard{};
        simulation::client::set_step_callback([&generator] { generator.service(); });
    }
    void LinuxStepHardware::stopService() noexcept
    {
        simulation::client::Transaction guard{};
        simulation::client::set_step_callback({});
    }
    auto LinuxStepHardware::reset() noexcept -> bool
    {
        m_valid = {};
        m_cut.reset();
        m_publicationError = false;
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
    auto LinuxStepHardware::sample() noexcept -> StepSample
    {
        auto result{ simulation::client::sample() };
        result.error |= m_publicationError;
        return result;
    }
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
    void LinuxStepHardware::retarget(std::size_t axis, std::uint32_t tick) noexcept
    {
        m_publicationError |=
          simulation::client::command(simulation::Operation::Retarget, { axis, tick, m_cut.value_or(0) })
            .status != 0;
    }
    auto LinuxStepHardware::publish() noexcept -> void
    {
        auto* shared{ simulation::client::memory() };
        if (!shared) {
            return;
        }
        for (unsigned axis{}; axis < 3; ++axis) {
            for (unsigned bank{}; bank < 2; ++bank) {
                if (m_valid[axis][bank] && m_memory[axis][bank] == m_published[axis][bank]) {
                    continue;
                }
                // STEP banks contain whole little-endian uint32_t words. Use
                // the fixed-size bank directly, so noexcept publication cannot
                // escape through the general protocol encoder's range exception.
                StepBuffer encoded{ m_memory[axis][bank] };
                if constexpr (std::endian::native == std::endian::big) {
                    for (auto& word : encoded) {
                        word = std::byteswap(word);
                    }
                }
                if (simulation::client::publish_bank(
                      axis, bank, std::as_bytes(std::span{ encoded }), m_cut)) {
                    m_published[axis][bank] = m_memory[axis][bank];
                    m_valid[axis][bank] = true;
                }
                else {
                    m_publicationError = true;
                }
            }
        }
    }

}
