#include "RgbOutput.hpp"
#include "hal/linux/DisplayHost.hpp"
#include <algorithm>
#include <tx_api.h>
namespace hal
{
    RgbOutput::~RgbOutput() { shutdown(); }
    auto RgbOutput::initialize() -> util::Result<>
    {
        if (!m_initialized && !linux::display::acquire()) {
            return std::unexpected(std::make_error_code(std::errc::no_such_device));
        }
        m_initialized = true;
        return {};
    }
    auto RgbOutput::framebuffer(unsigned slot) -> std::span<std::uint16_t>
    {
        return linux::display::framebuffer(slot);
    }
    auto RgbOutput::state() const noexcept -> display::State { return linux::display::snapshot(); }
    void RgbOutput::setCallback(display::Callback callback) noexcept
    {
        std::scoped_lock lock(m_callbackMutex);
        m_callback = callback;
    }
    void RgbOutput::shutdown() noexcept
    {
        setCallback({});
        if (m_initialized) {
            linux::display::release();
            m_initialized = false;
        }
    }
    auto RgbOutput::requestPresent(unsigned slot) -> util::Result<>
    {
        if (!linux::display::submit(slot)) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        return {};
    }
    auto RgbOutput::present(unsigned slot) -> util::Result<>
    {
        const auto result = requestPresent(slot);
        if (!result) {
            return result;
        }
        for (unsigned i = 0; i < 100; ++i) {
            service();
            const auto current = state();
            if (!current.pending) {
                return current.active == slot ? util::Result<>{}
                                              : std::unexpected(std::make_error_code(std::errc::io_error));
            }
            tx_thread_sleep(1);
        }
        return std::unexpected(std::make_error_code(std::errc::timed_out));
    }
    void RgbOutput::service() noexcept
    {
        const auto current = state();
        std::scoped_lock lock(m_callbackMutex);
        if (current.presentations != m_observed.presentations) {
            m_callback(display::Event::Presented);
        }
        if (current.vsyncs != m_observed.vsyncs) {
            m_callback(display::Event::VSync);
        }
        if (current.errors != m_observed.errors) {
            m_callback(display::Event::Error);
        }
        if (current.closed && !m_observed.closed) {
            m_callback(display::Event::Close);
        }
        m_observed = current;
    }
    auto RgbOutput::fill(unsigned slot, std::uint16_t color) -> util::Result<>
    {
        const auto current = state();
        if (slot > 1 || slot == current.active || current.pending == slot) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        std::ranges::fill(framebuffer(slot), color);
        return {};
    }
    auto RgbOutput::backlight(unsigned percent) -> util::Result<>
    {
        if (percent > 100) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        linux::display::brightness(percent);
        return {};
    }
}
