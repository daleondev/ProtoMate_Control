#include "LegacyStepHardware.hpp"
#include <thread>

namespace hal::util
{
    struct LegacyStepHardware::Service
    {
        explicit Service(StepGenerator& generator)
          : worker{ [&generator](const std::stop_token& stop) {
              while (!stop.stop_requested()) {
                  generator.service();
                  std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
              }
          } }
        {
        }
        std::jthread worker;
    };
    LegacyStepHardware::LegacyStepHardware() { trace_edges = false; }
    LegacyStepHardware::~LegacyStepHardware() { stopService(); }
    void LegacyStepHardware::beginService(StepGenerator& generator)
    {
        m_service = std::make_unique<Service>(generator);
    }
    void LegacyStepHardware::stopService() noexcept { m_service.reset(); }

    auto LegacyStepHardware::start(std::uint32_t tick) noexcept -> void
    {
        m_last = std::chrono::steady_clock::now();
        SimulatedStepHardware::start(tick);
    }

    auto LegacyStepHardware::sample() noexcept -> StepSample
    {
        update();
        return SimulatedStepHardware::sample();
    }

    auto LegacyStepHardware::stop() noexcept -> StepSample
    {
        update();
        return SimulatedStepHardware::stop();
    }

    auto LegacyStepHardware::update() noexcept -> void
    {
        if (m_advancing || !registers.running) {
            return;
        }
        m_advancing = true;
        const auto now{ std::chrono::steady_clock::now() };
        const auto ticks{ std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_last).count() /
                          STEP_TICK_NS };
        if (ticks > 0) {
            m_last += std::chrono::nanoseconds{ ticks * STEP_TICK_NS };
            advance(static_cast<std::uint64_t>(ticks));
        }
        m_advancing = false;
    }
}
