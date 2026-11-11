#include "GuiBackend.hpp"
#include "ui/opcua/Network.hpp"
#include "ui/opcua/Server.hpp"
namespace ui
{
    namespace hmi = gui::hmi;
    namespace
    {
        constexpr auto JOG_INPUT_DEADLINE{ std::chrono::milliseconds{ 200 } };
        constexpr auto INPUT_MAX_AGE{ std::chrono::milliseconds{ 100 } };
    }
    void GuiBackend::connection(icc::ConnectionStatus state, const char* detail)
    {
        m_services.connection(state, detail);
        if (state.state == icc::ConnectionState::Failed || state.reason == icc::ConnectionReason::PeerStopped) release();
    }
    void GuiBackend::read(hmi::Snapshot& result)
    {
        m_services.read(result);
        if (const auto next{ m_services.nextOutcome(m_outcomeSequence, 0) }) {
            m_outcomeSequence = next->sequence;
            result.outcome = *next;
        }
    }
    hmi::Outcome GuiBackend::submit(const hmi::Command& command)
    {
        if (command.kind == hmi::Kind::Disable || command.kind == hmi::Kind::Stop || command.kind == hmi::Kind::Abort) release();
        return m_services.submit(command);
    }
    unsigned GuiBackend::jobs(unsigned offset, std::span<hmi::Job> output, bool previews)
    { return m_services.jobs(offset, output, previews); }
    void GuiBackend::diagnostics(unsigned offset, hmi::DiagnosticPage& result)
    { m_services.diagnostics(offset, result); }
    hmi::Outcome GuiBackend::hold(const hmi::Command& c, hmi::layout::Bounds bounds)
    {
        const auto [x, y, w, h]{ bounds };
        release();
        std::uint64_t gesture{};
        {
            std::scoped_lock lock{ m_mutex };
            m_holding = true;
            gesture = ++m_gesture;
            m_x = x;
            m_y = y;
            m_width = w;
            m_height = h;
            m_inputDeadline = std::chrono::steady_clock::now() + JOG_INPUT_DEADLINE;
        }
        return submitHeld(c, gesture);
    }
    hmi::Outcome GuiBackend::submitHeld(const hmi::Command& c, std::uint64_t gesture)
    {
        auto result{ submit(c) };
        bool abandoned{};
        {
            std::scoped_lock lock{ m_mutex };
            const auto now{ std::chrono::steady_clock::now() };
            abandoned = !m_holding || gesture != m_gesture || now >= m_inputDeadline;
            if (!abandoned && result.accepted) {
                m_holdJob = result.job;
                m_client.superviseJog(result.job);
            }
            else if (gesture == m_gesture) {
                m_holding = false;
            }
        }
        if (abandoned && result.job) {
            m_client.cancelAsync(result.job);
        }
        return result;
    }
    void GuiBackend::input(const hmi::Input& input)
    {
        std::scoped_lock lock{ m_mutex };
        if (!m_holding) {
            return;
        }
        const auto now{ std::chrono::steady_clock::now() };
        const bool stop{ now >= m_inputDeadline || !input.valid || !input.down || input.x < m_x ||
                         input.y < m_y || input.x >= m_x + m_width || input.y >= m_y + m_height ||
                         now - input.sampled > INPUT_MAX_AGE };
        if (stop) {
            m_holding = false;
            const auto job{ m_holdJob };
            m_holdJob = 0;
            ++m_gesture;
            m_client.releaseJogInput();
            if (job) {
                m_client.cancelAsync(job);
            }
        }
        else {
            m_inputDeadline = now + JOG_INPUT_DEADLINE;
            if (m_holdJob) {
                m_client.renewJogInput(m_holdJob);
            }
        }
    }
    void GuiBackend::release()
    {
        {
            std::scoped_lock lock{ m_mutex };
            m_holding = false;
            const auto job{ m_holdJob };
            m_holdJob = 0;
            ++m_gesture;
            m_client.releaseJogInput();
            if (job) {
                m_client.cancelAsync(job);
            }
        }
    }
}

namespace ui
{
    gui::hmi::NetworkStatus GuiBackend::networkStatus() const
    {
        if (!m_network || !m_server) return {};
        const auto network{m_network->status()};
        const auto server{m_server->status()};
        const auto scan{m_network->scanStatus()};
        return {.wifi = opcua::Network::wifiSupported(),
            .connected = network.state == opcua::Network::State::Connected, .busy = network.busy,
            .saved = network.saved, .server = server.running, .remote = m_services.remotePermission(),
            .scanning = scan.running, .clients = server.clients, .scan_count = scan.count, .scan_omitted = scan.omitted,
            .error = network.result, .scan_error = scan.result, .address = network.address, .ssid = network.ssid};
    }
    bool GuiBackend::networkEntry(unsigned offset, gui::hmi::NetworkEntry& out) const
    {
        if (!m_network) return false;
        const auto scan{m_network->scanStatus()};
        if (offset >= scan.count) return false;
        const auto& row{scan.entries[offset]};
        out = {.ssid = row.ssid, .signal = row.signal, .channel = row.channel};
        return true;
    }
    bool GuiBackend::configureNetwork(std::string_view ssid, std::string_view password)
    {
        return m_network && m_network->configure(ssid, password, true);
    }
    bool GuiBackend::networkAction(gui::hmi::NetworkAction action)
    {
        using enum gui::hmi::NetworkAction;
        if (!m_network || !m_server) return false;
        switch (action) {
            case Scan: return m_network->scan();
            case Connect: return m_network->connect();
            case Disconnect: return m_network->disconnect();
            case Forget: return m_network->forget();
            case StartServer: m_server->enabled(true); return true;
            case StopServer: m_server->enabled(false); return true;
            case AllowRemote: m_services.remotePermission(true); return true;
            case RevokeRemote: m_services.remotePermission(false); return true;
        }
        return false;
    }
}
