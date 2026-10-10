#pragma once
#include "../Codec.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <pneumo/coroutines.hpp>

namespace p2p
{
    using EndpointId = std::uint16_t;
    using RequestId = std::uint64_t;
    enum class LinkState
    {
        Starting,
        Connected,
        Disconnected,
        TimedOut,
        ProtocolError,
        Closed
    };
    struct Options
    {
        std::size_t max_pending{ 8 };
        std::size_t max_routes{ 32 };
        std::chrono::milliseconds heartbeat{ 100 }, link_timeout{ 500 };
        // A hardware backend supplies a boot-unique value; zero selects a process-local ID.
        std::uint64_t session{};
    };
    namespace util
    {
        using Clock = std::chrono::steady_clock;
        enum class Kind : std::uint8_t
        {
            Hello,
            Heartbeat,
            Topic,
            Request,
            Reply,
            Goal,
            Accepted,
            Feedback,
            Result,
            Cancel,
            Ack,
            HelloAck
        };
        struct Frame
        {
            Kind kind{};
            std::uint8_t code{};
            EndpointId endpoint{};
            RequestId id{};
            std::uint64_t source{}, destination{};
            std::uint32_t budget_ms{};
            Bytes payload{};
        };
        Bytes pack(const Frame&);
        Frame unpack(std::span<const std::byte>);
        inline std::uint32_t budget(Clock::duration remaining)
        {
            auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            return static_cast<std::uint32_t>(std::clamp<std::int64_t>(ms, 0, 0xffffffffLL));
        }
        struct Route
        {
            virtual ~Route() = default;
            virtual void pump() = 0;
            virtual void receive(const Frame&) = 0;
            virtual void disconnect() = 0;
            virtual Clock::time_point deadline() const { return Clock::time_point::max(); }
        };
        struct Core final : std::enable_shared_from_this<Core>
        {
            Core(pnm::msg::Bus&, pnm::coro::Context&, std::shared_ptr<ITransport>, Options);
            ~Core();
            pnm::msg::Bus& bus;
            pnm::coro::Context& context;
            std::shared_ptr<ITransport> transport;
            const Options options;
            std::map<EndpointId, std::unique_ptr<Route>> routes;
            std::function<void(LinkState)> changed;
            std::atomic<LinkState> state{ LinkState::Starting };
            std::atomic_bool started{}, closing{}, queued{};
            std::uint64_t session{}, peer{};
            RequestId next_id{ 1 };
            std::size_t route_cursor{};
            Clock::time_point last_received{}, next_heartbeat{};
            pnm::coro::Context::Timer timer;
            bool hello_pending{ true }, acknowledgement_pending{};
            std::function<void()> wakeup();
            void wake() noexcept;
            void start();
            void close();
            void drain() noexcept;
            void fail(LinkState);
            bool send(Frame frame, Lane lane = Lane::Normal);
            void registerRoute(EndpointId id, std::unique_ptr<Route> route);
            RequestId allocateId();
            static pnm::coro::Task<void> dispatch(std::shared_ptr<Core> core);

          private:
            void transition(LinkState value);
            void receive(const Frame&);
        };
    }
}
