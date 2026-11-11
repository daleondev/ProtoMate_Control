#pragma once
#include "Layout.hpp"
#include "Text.hpp"
#include "Network.hpp"
#include "ui/app/services/Types.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <span>

namespace ui::gui::hmi
{
    using services::DIAGNOSTIC_ROWS;
    using services::GRAPHICS_METRICS;
    using services::INITIAL_GRAPHICS_VALUE;
    using services::DEFAULT_LATCH_SPEED;
    using services::Severity;
    using services::Kind;
    using services::Command;
    using services::Outcome;
    using services::Axis;
    using services::Snapshot;
    using services::Job;
    using services::DiagnosticRow;
    using services::DiagnosticPage;
    struct Input
    {
        bool down{}, valid{};
        int x{}, y{};
        std::chrono::steady_clock::time_point sampled{};
    };
    class Backend
    {
      public:
        Backend() = default;
        Backend(const Backend&) = delete;
        auto operator=(const Backend&) -> Backend& = delete;
        Backend(Backend&&) = delete;
        auto operator=(Backend&&) -> Backend& = delete;

        virtual ~Backend() = default;
        virtual void read(Snapshot& result) = 0;
        virtual Outcome submit(const Command& command) = 0;
        virtual unsigned jobs(unsigned offset, std::span<Job> output, bool previews) = 0;
        // Begin from the GUI, supervise from the acquisition/client workers.
        virtual Outcome hold(const Command& command, layout::Bounds bounds) = 0;
        virtual void input(const Input& input) = 0;
        virtual void release() = 0;
        virtual void diagnostics(unsigned /*unused*/, DiagnosticPage& result) { result = {}; }
        virtual void recordMessage(const char*, Severity) {}
        virtual std::uint64_t messageRevision() const { return 0; }
        virtual bool message(unsigned, services::Message&) { return false; }
        virtual void acknowledgeMessages() {}
        virtual NetworkStatus networkStatus() const { return {}; }
        virtual bool networkEntry(unsigned, NetworkEntry&) const { return false; }
        virtual bool networkAction(NetworkAction) { return false; }
        virtual bool configureNetwork(std::string_view, std::string_view) { return false; }
    };
}
