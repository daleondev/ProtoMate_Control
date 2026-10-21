#include "SmartwinDisplay.hpp"
#include <algorithm>
#include <chrono>
namespace hal::device
{
    SmartwinDisplay::SmartwinDisplay(std::shared_ptr<IRgbOutput> output,
                                     std::shared_ptr<IDigitalOutput> standby,
                                     std::shared_ptr<IDigitalOutput> reset,
                                     std::shared_ptr<IDigitalInput> powerReady)
      : m_output(std::move(output))
      , m_standby(std::move(standby))
      , m_reset(std::move(reset))
      , m_powerReady(std::move(powerReady))
    {
    }
    SmartwinDisplay::~SmartwinDisplay() { shutdown(); }
    void SmartwinDisplay::shutdown() noexcept
    {
        m_output->setCallback({});
        if (m_initialized) {
            static_cast<void>(m_output->backlight(0));
        }
        m_output->shutdown();
        m_standby->write(gpio::Level::Low);
        m_reset->write(gpio::Level::Low);
        m_initialized = false;
        m_phase = Phase::Unpowered;
        m_brightness = 0;
    }
    auto SmartwinDisplay::initialize() -> util::Result<>
    {
        if (m_initialized) {
            return {};
        }
        m_standby->write(gpio::Level::Low);
        m_reset->write(gpio::Level::Low);
        const auto result = m_output->initialize();
        if (!result) {
            return result;
        }
        m_initialized = true;
        m_phase = Phase::Unpowered;
        service(); // LTDC and the GUI run even when the adapter is unpowered/absent.
        return {};
    }
    auto SmartwinDisplay::backlight(unsigned percent) -> util::Result<>
    {
        if (percent > 100) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        m_brightness = percent;
        return m_output->backlight(m_phase == Phase::Active ? percent : 0);
    }
    auto SmartwinDisplay::servicePeriod() const noexcept -> std::chrono::milliseconds
    {
        return std::min(m_output->servicePeriod(), std::chrono::milliseconds{10});
    }
    void SmartwinDisplay::service() noexcept
    {
        using namespace std::chrono_literals;
        m_output->service();
        if (!m_initialized) {
            return;
        }
        // Hardware gates the outputs immediately. This bounded thread-context
        // state machine also repeats panel initialization after power returns.
        const bool ready = !m_powerReady || m_powerReady->read() == gpio::Level::High;
        const auto now = std::chrono::steady_clock::now();
        if (!ready) {
            if (m_phase != Phase::Unpowered) {
                static_cast<void>(m_output->backlight(0));
                m_standby->write(gpio::Level::Low);
                m_reset->write(gpio::Level::Low);
                m_phase = Phase::Unpowered;
            }
            return;
        }
        switch (m_phase) {
        case Phase::Unpowered:
            m_deadline = now + 2ms;
            m_phase = Phase::ResetHold;
            break;
        case Phase::ResetHold:
            if (now >= m_deadline) {
                m_reset->write(gpio::Level::High);
                m_deadline = now + 10ms;
                m_phase = Phase::ResetReleased;
            }
            break;
        case Phase::ResetReleased:
            if (now >= m_deadline) {
                m_standby->write(gpio::Level::High);
                m_deadline = now + 60ms;
                m_phase = Phase::Starting;
            }
            break;
        case Phase::Starting:
            if (now >= m_deadline) {
                m_frameSync = m_output->state().vsyncs;
                m_phase = Phase::AwaitFrame;
            }
            break;
        case Phase::AwaitFrame:
            if (const auto state = m_output->state(); state.presentations && state.vsyncs != m_frameSync && !state.errors) {
                static_cast<void>(m_output->backlight(m_brightness));
                m_phase = Phase::Active;
            }
            break;
        case Phase::Active:
            break;
        }
    }
}
