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
    enum class LinkState : std::uint8_t
    {
        Starting,
        Connected,
        Disconnected,
        TimedOut,
        ProtocolError,
        Closed
    };
    inline constexpr std::size_t DEFAULT_MAX_PENDING = 8;
    inline constexpr std::size_t DEFAULT_MAX_ROUTES = 32;
    inline constexpr std::chrono::milliseconds DEFAULT_HEARTBEAT{ 100 };
    inline constexpr std::chrono::milliseconds DEFAULT_LINK_TIMEOUT{ 500 };

    struct Options
    {
        std::size_t max_pending{ DEFAULT_MAX_PENDING };
        std::size_t max_routes{ DEFAULT_MAX_ROUTES };
        std::chrono::milliseconds heartbeat{ DEFAULT_HEARTBEAT }, link_timeout{ DEFAULT_LINK_TIMEOUT };
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
            Bytes payload;
        };
        Bytes pack(const Frame& f);
        Frame unpack(std::span<const std::byte> bytes);
        inline std::uint32_t budget(Clock::duration remaining)
        {
            auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            return static_cast<std::uint32_t>(
              std::clamp<std::int64_t>(ms, 0, std::numeric_limits<std::uint32_t>::max()));
        }
        struct Route
        {
            Route() = default;
            Route(const Route&) = delete;
            Route& operator=(const Route&) = delete;
            Route(Route&&) = delete;
            Route& operator=(Route&&) = delete;
            virtual ~Route() = default;
            virtual void pump() = 0;
            virtual void receive(const Frame&) = 0;
            virtual void disconnect() = 0;
            virtual Clock::time_point deadline() const { return Clock::time_point::max(); }
        };
        struct Core final : std::enable_shared_from_this<Core>
        {
            Core(pnm::msg::Bus& b, pnm::coro::Context& c, std::shared_ptr<ITransport> t, Options o);
            ~Core();
            Core(const Core&) = delete;
            Core& operator=(const Core&) = delete;
            Core(Core&&) = delete;
            Core& operator=(Core&&) = delete;
            pnm::msg::Bus& bus;
            pnm::coro::Context& context;
            std::shared_ptr<ITransport> transport;
            const Options options;
            std::map<EndpointId, std::unique_ptr<Route>> routes;
            std::function<void(LinkState)> changed;
            std::atomic<LinkState> state{ LinkState::Starting };
            std::atomic_bool started, closing, queued;
            std::uint64_t session{}, peer{};
            RequestId next_id{ 1 };
            std::size_t route_cursor{};
            Clock::time_point last_received, next_heartbeat;
            pnm::coro::Context::Timer timer;
            bool hello_pending{ true }, acknowledgement_pending{};
            std::function<void()> wakeup();
            void wake() noexcept;
            void start();
            void close();
            void drain() noexcept;
            void fail(LinkState reason);
            bool send(Frame frame, Lane lane = Lane::Normal);
            void registerRoute(EndpointId id, std::unique_ptr<Route> route);
            RequestId allocateId();
            static pnm::coro::Task<void> dispatch(std::shared_ptr<Core> core);

          private:
            void receiveFrames();
            void updateLink();
            void pumpRoutes();
            void scheduleNext();
            void transition(LinkState value);
            void receive(const Frame& frame);
        };
    }
}
