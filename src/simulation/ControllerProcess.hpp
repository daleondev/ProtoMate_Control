#pragma once
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
        ~ControllerProcess();
        ControllerProcess() = default;
        ControllerProcess(const ControllerProcess&) = delete;
        ControllerProcess& operator=(const ControllerProcess&) = delete;
        void launch(int image, std::span<char*> arguments, const std::array<int, 5>& descriptors);
        bool running() const noexcept { return m_child > 0; }
        std::optional<int> poll() noexcept;
        int finish(std::chrono::milliseconds grace = std::chrono::milliseconds{ 2000 }) noexcept;

      private:
        int m_child{ -1 };
        bool m_launched{};
        std::optional<int> m_result;
    };
}
