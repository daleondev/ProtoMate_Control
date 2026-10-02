#pragma once

#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/hal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace hal
{
    class GpioInput final : public IDigitalInput
    {
      public:
        explicit GpioInput(gpio::InputConfiguration configuration);
        ~GpioInput() override;

        GpioInput(const GpioInput&) = delete;
        GpioInput& operator=(const GpioInput&) = delete;
        GpioInput(GpioInput&&) = delete;
        GpioInput& operator=(GpioInput&&) = delete;

        [[nodiscard]] auto read() const noexcept -> gpio::Level override;
        auto setEdgeCallback(EdgeCallback callback) noexcept -> void override;

        [[nodiscard]] static auto interruptLineAvailable(gpio::Pin pin) noexcept -> bool;
        static auto dispatchEdge(std::uint16_t pin_mask) noexcept -> void;

      private:
        gpio::InputConfiguration m_configuration;
        GPIO_TypeDef* m_port;
        std::uint16_t m_pinMask;
        IRQn_Type m_interrupt;
        EdgeCallback m_edgeCallback;

        inline static std::array<GpioInput*, 16U> s_interruptRegistry{};
    };

    class GpioOutput final : public IDigitalOutput
    {
      public:
        explicit GpioOutput(gpio::OutputConfiguration configuration);
        ~GpioOutput() override;

        GpioOutput(const GpioOutput&) = delete;
        GpioOutput& operator=(const GpioOutput&) = delete;
        GpioOutput(GpioOutput&&) = delete;
        GpioOutput& operator=(GpioOutput&&) = delete;

        [[nodiscard]] auto read() const noexcept -> gpio::Level override;
        auto write(gpio::Level level) noexcept -> void override;
        auto toggle() noexcept -> void override;

      private:
        GPIO_TypeDef* m_port;
        std::uint16_t m_pinMask;
    };
}
