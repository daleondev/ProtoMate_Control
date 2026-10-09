#include "StepHardware.hpp"
#include <thread>

namespace hal::util
{
    struct LinuxStepHardware::Service
    {
        explicit Service(StepGenerator& generator)
          : worker{ [&generator](std::stop_token stop) {
              while (!stop.stop_requested()) {
                  generator.service();
                  std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
              }
          } }
        {
        }
        std::jthread worker;
    };
    LinuxStepHardware::LinuxStepHardware() { trace_edges = false; }
    LinuxStepHardware::~LinuxStepHardware() { stopService(); }
    void LinuxStepHardware::beginService(StepGenerator& generator)
    {
        m_service = std::make_unique<Service>(generator);
    }
    void LinuxStepHardware::stopService() noexcept { m_service.reset(); }

    auto LinuxStepHardware::start(std::uint32_t tick) noexcept -> void
    {
        m_last = std::chrono::steady_clock::now();
        SimulatedStepHardware::start(tick);
    }

    auto LinuxStepHardware::sample() noexcept -> StepSample
    {
        update();
        return SimulatedStepHardware::sample();
    }

    auto LinuxStepHardware::stop() noexcept -> StepSample
    {
        update();
        return SimulatedStepHardware::stop();
    }

    auto LinuxStepHardware::update() noexcept -> void
    {
        if (m_advancing || !registers.running) {
            return;
        }
        m_advancing = true;
        const auto now{ std::chrono::steady_clock::now() };
        const auto ticks{ std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_last).count() /
                          step_tick_ns };
        if (ticks > 0) {
            m_last += std::chrono::nanoseconds{ ticks * step_tick_ns };
            advance(static_cast<std::uint64_t>(ticks));
        }
        m_advancing = false;
    }
}
