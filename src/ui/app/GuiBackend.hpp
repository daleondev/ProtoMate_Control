#pragma once
#include "Services.hpp"
#include "ui/gui/hmi/Backend.hpp"
namespace ui::opcua { class Network; class Server; }
namespace ui
{
    // Only local pointer/gesture handling lives in the GUI adapter.
    class GuiBackend final : public gui::hmi::Backend
    {
      public:
        GuiBackend(Services& services, motion::Client& client) : m_services{ services }, m_client{ client } {}
        void connection(icc::ConnectionStatus state, const char* detail = nullptr);
        void read(gui::hmi::Snapshot& result) override;
        gui::hmi::Outcome submit(const gui::hmi::Command& command) override;
        unsigned jobs(unsigned offset, std::span<gui::hmi::Job> output, bool previews) override;
        gui::hmi::Outcome hold(const gui::hmi::Command& command, gui::hmi::layout::Bounds bounds) override;
        void input(const gui::hmi::Input& input) override;
        void release() override;
        void diagnostics(unsigned offset, gui::hmi::DiagnosticPage& result) override;
        void recordMessage(const char* text, services::Severity severity) override { m_services.message(text, severity); }
        std::uint64_t messageRevision() const override { return m_services.messageRevision(); }
        bool message(unsigned offset, services::Message& result) override { return m_services.messages(offset, {&result, 1}) != 0; }
        void acknowledgeMessages() override { m_services.acknowledgeMessages(UINT64_MAX); }
        void networking(opcua::Network& network, opcua::Server& server) { m_network = &network; m_server = &server; }
        gui::hmi::NetworkStatus networkStatus() const override;
        bool networkEntry(unsigned, gui::hmi::NetworkEntry&) const override;
        bool networkAction(gui::hmi::NetworkAction) override;
        bool configureNetwork(std::string_view ssid, std::string_view password) override;
      private:
        gui::hmi::Outcome submitHeld(const gui::hmi::Command& command, std::uint64_t gesture);
        Services& m_services;
        motion::Client& m_client;
        opcua::Network* m_network{};
        opcua::Server* m_server{};
        std::mutex m_mutex;
        std::uint64_t m_gesture{}, m_holdJob{};
        std::uint64_t m_outcomeSequence{};
        int m_x{}, m_y{}, m_width{}, m_height{};
        bool m_holding{};
        std::chrono::steady_clock::time_point m_inputDeadline{};
    };
}
