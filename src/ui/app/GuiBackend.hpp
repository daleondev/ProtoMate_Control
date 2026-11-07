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
        GuiBackend(pnm::msg::Bus&, motion::Client&, motion::Robot&, SystemState&);
        ~GuiBackend() override;
        void connection(icc::ConnectionStatus, const char* detail = nullptr);
        void read(gui::hmi::Snapshot&) override;
        gui::hmi::Outcome submit(const gui::hmi::Command&) override;
        unsigned jobs(unsigned, std::span<gui::hmi::Job>, bool) override;
        gui::hmi::Outcome hold(const gui::hmi::Command&, int, int, int, int) override;
        void input(const gui::hmi::Input&) override;
        void release() override;
        void diagnostics(unsigned, gui::hmi::DiagnosticPage&) override;

      private:
        void update(); // existing client worker
        void outcome(std::uint64_t id, bool ok, const char* text, std::uint64_t job = 0);
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
        std::optional<unsigned> m_memoryRequest;
        bool m_memoryBusy{};
        pnm::msg::ReadinessRegistration m_memoryReady;
        std::uint64_t m_memoryOperation{};
        diagnostics::MemorySnapshot m_memoryData{};
        std::chrono::steady_clock::time_point m_memoryTime{};
        bool m_memoryAvailable{}, m_memoryIO{};
        gui::hmi::Text<96> m_memoryError{};
    };
}
