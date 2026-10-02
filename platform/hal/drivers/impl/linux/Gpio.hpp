#pragma once

#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"

#include <pthread.h>

#include <atomic>
#include <memory>

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

        // Simulation input used by Linux integration tests and test applications.
        auto setSimulatedLevel(gpio::Level level) noexcept -> void;

      private:
        [[nodiscard]] auto edgeMatches(gpio::Level previous, gpio::Level current) const noexcept -> bool;

        gpio::InputConfiguration m_configuration;
        std::atomic<gpio::Level> m_level{ gpio::Level::Low };
        pthread_mutex_t m_callbackMutex{};
        std::shared_ptr<EdgeCallback> m_edgeCallback;
    };

    class GpioOutput final : public IDigitalOutput
    {
      public:
        explicit GpioOutput(gpio::OutputConfiguration configuration) noexcept;

        [[nodiscard]] auto read() const noexcept -> gpio::Level override;
        auto write(gpio::Level level) noexcept -> void override;
        auto toggle() noexcept -> void override;

      private:
        std::atomic<gpio::Level> m_level;
    };
}
