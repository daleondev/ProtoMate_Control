#pragma once
#include "SystemState.hpp"
#include "common/diagnostics/Memory.hpp"
#include "services/Types.hpp"
#include "ui/motion/Client.hpp"
#include "ui/motion/Robot.hpp"

namespace ui
{
    // Shared, nonblocking application boundary for frontends. One client, planner
    // and diagnostic cache; GUI input geometry belongs in GuiBackend.
    class Services final
    {
      public:
        Services(const Services&) = delete;
        auto operator=(const Services&) -> Services& = delete;
        Services(Services&&) = delete;
        auto operator=(Services&&) -> Services& = delete;

        Services(pnm::msg::Bus& /*bus*/,
                   motion::Client& /*client*/,
                   motion::Robot& /*robot*/,
                   SystemState& /*system*/);
        ~Services();
        void connection(icc::ConnectionStatus /*state*/, const char* detail = nullptr);
        void read(services::Snapshot& /*result*/);
        services::Outcome submit(const services::Command& command, std::uint64_t source = 0);
        std::optional<services::Outcome> operation(std::uint64_t id, std::uint64_t source) const;
        std::optional<services::Outcome> nextOutcome(std::uint64_t after, std::uint64_t source) const;
        // Subscribers own their notification. Expired subscriptions release a
        // bounded slot; no frontend callback runs inside the application worker.
        bool subscribe(const std::shared_ptr<runtime::Notification>& notification);
        void remotePermission(bool enabled) { m_client.remotePermission(enabled); }
        bool remotePermission() const { return m_client.remotePermission(); }
        std::uint64_t openRemote() { return m_client.openRemote(); }
        bool renewRemote(std::uint64_t source, std::span<const std::uint64_t> held) { return m_client.renewRemote(source, held); }
        void closeRemote(std::uint64_t source) { m_client.closeRemote(source); }
        bool remoteAuthorized(std::uint64_t source) const { return m_client.remoteAuthorized(source); }
        unsigned jobs(unsigned /*offset*/, std::span<services::Job> /*output*/, bool /*previews*/);
        unsigned detailedJobs(unsigned offset, std::span<services::DetailedJob> output, bool previews);
        void diagnostics(unsigned /*unused*/, services::DiagnosticPage& /*result*/);
        bool refreshDiagnostics(bool io, bool stacks);
        void message(const char* text, services::Severity severity, std::uint64_t operation = 0);
        unsigned messages(unsigned offset, std::span<services::Message> output, std::uint64_t* revision = nullptr) const;
        std::uint64_t messageRevision() const;
        void acknowledgeMessages(std::uint64_t through);
        // Installed once by composition before workers start. The GUI remains
        // the only physical display owner; these hooks enqueue/read bounded data.
        void graphicsHooks(std::function<services::Graphics()> read, std::function<bool(unsigned)> brightness);
        services::Graphics graphics() const;
        services::QueueDiagnostics queues() const;
        services::Configuration configuration() const;
        void networkHook(std::function<services::NetworkDiagnostics()> read) {m_network = std::move(read);}
        services::NetworkDiagnostics network() const {return m_network ? m_network() : services::NetworkDiagnostics{};}

      private:
        void update(); // existing client worker
        void updateMemory();
        unsigned previewJobs(unsigned offset, std::span<services::Job> output);
        void requestMemory(const services::Command& c, std::uint64_t operation);
        std::uint64_t submitPreview(const services::Command& c, const services::Snapshot& state, std::uint64_t source);
        std::uint64_t submitCartesianJog(const services::Command& c, std::uint64_t source);
        std::uint64_t submitRobot(const services::Command& c, std::uint64_t source);
        std::uint64_t submitAxis(const services::Command& c, std::uint64_t source);
        std::uint64_t submitRaw(const services::Command& c, const services::Snapshot& state, std::uint64_t source);
        void submitControl(std::uint64_t id, const ::motion::MotorRequest& request, std::uint64_t source);
        void outcome(std::uint64_t id, bool ok, const char* message, std::uint64_t job = 0);
        std::uint64_t reserveOperation(std::uint64_t source, bool urgent);
        void publishOutcome(services::Outcome value, bool pending);
        void notifySubscribers();
        motion::Client& m_client;
        motion::Robot& m_robot;
        SystemState& m_system;
        pnm::msg::Service<diagnostics::MemoryQuery, diagnostics::MemorySnapshot> m_memory;
        std::optional<pnm::msg::PendingCall<diagnostics::MemorySnapshot>> m_memoryCall;
        mutable std::mutex m_mutex;
        struct Operation
        {
            services::Outcome value;
            std::uint64_t source{};
            bool pending{};
        };
        struct Storage
        {
            services::Snapshot state;
            diagnostics::MemorySnapshot memory;
            std::array<Operation, 20> operations;
            std::array<services::Message, 8> messages;
        };
        static_assert(std::is_trivially_copyable_v<Storage>);
        struct ReleaseStorage { void operator()(Storage* storage) const noexcept; };
        static std::unique_ptr<Storage, ReleaseStorage> reserveStorage();
        std::unique_ptr<Storage, ReleaseStorage> m_storage;
        std::array<std::weak_ptr<runtime::Notification>, 4> m_subscribers;
        std::uint64_t m_next{ 1 }, m_outcomeSequence{};
        std::uint64_t m_operationRejections{};
        unsigned m_pendingPeak{};
        std::uint64_t m_messageSequence{}, m_messageRevision{};
        std::function<services::Graphics()> m_graphics;
        std::function<bool(unsigned)> m_brightness;
        std::function<services::NetworkDiagnostics()> m_network;
        std::optional<unsigned> m_memoryRequest;
        bool m_memoryBusy{};
        pnm::msg::ReadinessRegistration m_memoryReady;
        std::uint64_t m_memoryOperation{};
        std::chrono::steady_clock::time_point m_memoryTime{};
        bool m_memoryAvailable{}, m_memoryIO{};
        services::Text<services::text_capacity::LABEL> m_memoryError{};
    };
}
