#pragma once
#include "simulation/Protocol.hpp"
#include <array>
#include <chrono>
#include <optional>
#include <span>

namespace simulation
{
    // Single owner: main thread until launch/start, then the session worker until
    // it is joined. Reaping precedes publication of the final session outcome.
    class ControllerProcess final
    {
      public:
        static constexpr auto SHUTDOWN_GRACE{ std::chrono::milliseconds{ 2000 } };
        ControllerProcess(ControllerProcess&&) = delete;
        auto operator=(ControllerProcess&&) -> ControllerProcess& = delete;

        ~ControllerProcess();
        ControllerProcess() = default;
        ControllerProcess(const ControllerProcess&) = delete;
        ControllerProcess& operator=(const ControllerProcess&) = delete;
        void launch(int executable_fd,
                    std::span<char*> arguments,
                    const std::array<int, DESCRIPTOR_COUNT>& descriptors);
        bool running() const noexcept { return m_child > 0; }
        std::optional<int> poll() noexcept;
        int finish(std::chrono::milliseconds grace = SHUTDOWN_GRACE) noexcept;

      private:
        int m_child{ -1 };
        bool m_launched{};
        std::optional<int> m_result;
    };
}
