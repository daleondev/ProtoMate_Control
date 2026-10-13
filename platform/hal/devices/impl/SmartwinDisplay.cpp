#include "SmartwinDisplay.hpp"
#include <thread>
#include <chrono>
namespace hal::device {
    SmartwinDisplay::SmartwinDisplay(std::shared_ptr<IRgbOutput> output,
        std::shared_ptr<IDigitalOutput> standby, std::shared_ptr<IDigitalOutput> reset)
      : m_output(std::move(output)), m_standby(std::move(standby)), m_reset(std::move(reset)) {}
    SmartwinDisplay::~SmartwinDisplay() {
        if (m_initialized) { static_cast<void>(m_output->backlight(0)); }
        m_standby->write(gpio::Level::Low);
        m_reset->write(gpio::Level::Low);
    }
    auto SmartwinDisplay::initialize() -> util::Result<> {
        using namespace std::chrono_literals;
        if (m_initialized) { return {}; }
        m_standby->write(gpio::Level::Low);
        m_reset->write(gpio::Level::Low);
        std::this_thread::sleep_for(2ms);
        const auto result = m_output->initialize();
        if (!result) { return result; }
        m_reset->write(gpio::Level::High);
        std::this_thread::sleep_for(10ms);
        m_standby->write(gpio::Level::High);
        std::this_thread::sleep_for(60ms);
        m_initialized = true;
        return {};
    }
}
