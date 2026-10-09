#include "Dm542tDriver.hpp"
#include <stdexcept>

namespace hal::device
{
    Dm542tDriver::Dm542tDriver(std::shared_ptr<IDigitalInput> alarm, std::shared_ptr<IDigitalOutput> enable)
      : m_enable{ std::move(enable) }, m_fault{ std::move(alarm) }
    {
        if (!m_enable) throw std::invalid_argument("shared driver enable unavailable");
    }
    hal::util::Result<> Dm542tDriver::initialize()
    {
        if (m_enable->read() != gpio::Level::High)
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        m_fault.beginRecovery();
        return m_fault.completeRecovery();
    }
    hal::util::Result<> Dm542tDriver::verify()
    {
        service();
        const auto state{ status() };
        if (!state.ready)
            return std::unexpected(state.error ? state.error : std::make_error_code(std::errc::operation_not_permitted));
        return {};
    }
}
