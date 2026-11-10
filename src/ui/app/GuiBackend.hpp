#pragma once
#include "SystemState.hpp"
#include "common/diagnostics/Memory.hpp"
#include "ui/gui/hmi/Backend.hpp"
#include "ui/motion/Client.hpp"
#include "ui/motion/Robot.hpp"

namespace ui
{
    // Application composition adapter. It shares the CLI's client/planner;
    // the portable GUI target has no dependency on either implementation.
    class GuiBackend final : public gui::hmi::Backend
    {
      public:
        GuiBackend(const GuiBackend&) = delete;
        auto operator=(const GuiBackend&) -> GuiBackend& = delete;
        GuiBackend(GuiBackend&&) = delete;
        auto operator=(GuiBackend&&) -> GuiBackend& = delete;

        GuiBackend(pnm::msg::Bus& /*bus*/,
                   motion::Client& /*client*/,
                   motion::Robot& /*robot*/,
                   SystemState& /*system*/);
        ~GuiBackend() override;
        void connection(icc::ConnectionStatus /*state*/, const char* detail = nullptr);
        void read(gui::hmi::Snapshot& /*result*/) override;
        gui::hmi::Outcome submit(const gui::hmi::Command& /*command*/) override;
        unsigned jobs(unsigned /*offset*/, std::span<gui::hmi::Job> /*output*/, bool /*previews*/) override;
        gui::hmi::Outcome hold(const gui::hmi::Command& /*command*/,
                               gui::hmi::layout::Bounds /*bounds*/) override;
        void input(const gui::hmi::Input& /*input*/) override;
        void release() override;
        void diagnostics(unsigned /*unused*/, gui::hmi::DiagnosticPage& /*result*/) override;

      private:
        void update(); // existing client worker
        void updateMemory();
        unsigned previewJobs(unsigned offset, std::span<gui::hmi::Job> output);
        void requestMemory(const gui::hmi::Command& c, std::uint64_t operation);
        std::uint64_t submitPreview(const gui::hmi::Command& c, const gui::hmi::Snapshot& state);
        std::uint64_t submitCartesianJog(const gui::hmi::Command& c);
        std::uint64_t submitRobot(const gui::hmi::Command& c);
        std::uint64_t submitAxis(const gui::hmi::Command& c);
        std::uint64_t submitRaw(const gui::hmi::Command& c, const gui::hmi::Snapshot& state);
        void submitControl(std::uint64_t id, const ::motion::MotorRequest& request);
        gui::hmi::Outcome submitHeld(const gui::hmi::Command& /*c*/, std::uint64_t gesture);
        void outcome(std::uint64_t id, bool ok, const char* message, std::uint64_t job = 0);
        motion::Client& m_client;
        motion::Robot& m_robot;
        SystemState& m_system;
        pnm::msg::Service<diagnostics::MemoryQuery, diagnostics::MemorySnapshot> m_memory;
        std::optional<pnm::msg::PendingCall<diagnostics::MemorySnapshot>> m_memoryCall;
        mutable std::mutex m_mutex;
        gui::hmi::Snapshot m_state;
        std::uint64_t m_next{ 1 }, m_gesture{}, m_holdJob{};
        int m_x{}, m_y{}, m_width{}, m_height{};
        bool m_holding{};
        std::chrono::steady_clock::time_point m_inputDeadline{};
        std::optional<unsigned> m_memoryRequest;
        bool m_memoryBusy{};
        pnm::msg::ReadinessRegistration m_memoryReady;
        std::uint64_t m_memoryOperation{};
        diagnostics::MemorySnapshot m_memoryData{};
        std::chrono::steady_clock::time_point m_memoryTime{};
        bool m_memoryAvailable{}, m_memoryIO{};
        gui::hmi::Text<gui::hmi::text_capacity::LABEL> m_memoryError{};
    };
}
