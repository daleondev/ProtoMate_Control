#include "Core.hpp"

namespace p2p::util
{
    namespace
    {
        constexpr std::uint32_t FRAME_MAGIC = 0x50325031;
        constexpr std::uint16_t PROTOCOL_VERSION = 1;
        constexpr std::uint64_t SESSION_STRIDE = 0x100000001ULL;
        constexpr unsigned MAX_DISPATCH_FRAMES = 64;
        std::uint64_t new_session()
        {
            static std::atomic<std::uint64_t> ids{ 1 };
            auto v = ids.fetch_add(1);
            if (!v)
                throw std::overflow_error("p2p session IDs exhausted");
            return v;
        }
    }
    Bytes pack(const Frame& f)
    {
        if (f.payload.size() > MAX_PAYLOAD)
            throw std::length_error("p2p payload exceeds limit");
        Writer w;
        w.integer(FRAME_MAGIC);
        w.integer(PROTOCOL_VERSION);
        w.integer<std::uint8_t>(static_cast<std::uint8_t>(f.kind));
        w.integer(f.code);
        w.integer(f.endpoint);
        w.integer(f.id);
        w.integer(f.source);
        w.integer(f.destination);
        w.integer(f.budget_ms);
        w.integer<std::uint16_t>(static_cast<std::uint16_t>(f.payload.size()));
        w.bytes.insert(w.bytes.end(), f.payload.begin(), f.payload.end());
        return std::move(w.bytes);
    }
    Frame unpack(std::span<const std::byte> bytes)
    {
        Reader r(bytes);
        if (r.integer<std::uint32_t>() != FRAME_MAGIC || r.integer<std::uint16_t>() != PROTOCOL_VERSION)
            throw std::invalid_argument("p2p protocol mismatch");
        Frame f;
        auto kind = r.integer<std::uint8_t>();
        if (kind > static_cast<unsigned>(Kind::HelloAck))
            throw std::invalid_argument("invalid p2p frame kind");
        f.kind = static_cast<Kind>(kind);
        f.code = r.integer<std::uint8_t>();
        f.endpoint = r.integer<EndpointId>();
        f.id = r.integer<RequestId>();
        f.source = r.integer<std::uint64_t>();
        f.destination = r.integer<std::uint64_t>();
        f.budget_ms = r.integer<std::uint32_t>();
        auto size = r.integer<std::uint16_t>();
        if (size > MAX_PAYLOAD || r.remaining().size() != size || !f.source)
            throw std::invalid_argument("invalid p2p frame length/session");
        f.payload.assign(r.remaining().begin(), r.remaining().end());
        return f;
    }
    Core::Core(pnm::msg::Bus& b, pnm::coro::Context& c, std::shared_ptr<ITransport> t, Options o)
      : bus(b)
      , context(c)
      , transport(std::move(t))
      , options(o)
      , session(o.session ? o.session : new_session())
    {
        if (!transport || !o.max_pending || !o.max_routes || o.heartbeat.count() <= 0 ||
            o.link_timeout <= o.heartbeat)
            throw std::invalid_argument("invalid p2p bridge options");
    }
    Core::~Core()
    {
        timer.cancel();
        transport->setWakeup({});
    }
    std::function<void()> Core::wakeup()
    {
        return [weak = weak_from_this()]() noexcept {
            if (auto self = weak.lock())
                self->wake();
        };
    }
    void Core::wake() noexcept
    {
        if (!started.load() || state.load() == LinkState::Closed || queued.exchange(true))
            return;
        try {
            pnm::coro::co_spawn(context,
                                [self = shared_from_this()](pnm::coro::Context&) { return dispatch(self); });
        } catch (...) {
            queued.store(false);
            closing.store(true);
            try {
                transition(LinkState::ProtocolError);
            } catch (...) {
                // A failing observer must not hide the locally latched fault.
                state.store(LinkState::ProtocolError);
            }
        }
    }
    // The Clang fork diagnoses compiler-generated coroutine awaiter calls here.
    // NOLINTBEGIN(readability-static-accessed-through-instance)
    pnm::coro::Task<void> Core::dispatch(std::shared_ptr<Core> core)
    {
        core->queued.store(false);
        core->drain();
        co_return;
    }
    // NOLINTEND(readability-static-accessed-through-instance)
    void Core::start()
    {
        if (started.exchange(true))
            throw std::logic_error("p2p bridge already started");
        last_received = Clock::now();
        next_heartbeat = last_received;
        transport->setWakeup(wakeup());
        wake();
    }
    void Core::close()
    {
        closing.store(true);
        if (!started.load()) {
            state.store(LinkState::Closed);
            routes.clear();
        }
        else
            wake();
    }
    void Core::registerRoute(EndpointId id, std::unique_ptr<Route> route)
    {
        if (started.load() || !id || routes.size() >= options.max_routes || routes.contains(id))
            throw std::logic_error("invalid/duplicate/late p2p route registration");
        routes.emplace(id, std::move(route));
    }
    RequestId Core::allocateId()
    {
        if (!next_id)
            throw std::overflow_error("p2p request IDs exhausted");
        return next_id++;
    }
    void Core::transition(LinkState value)
    {
        auto old = state.exchange(value);
        if (old != value && changed)
            changed(value);
    }
    void Core::fail(LinkState reason)
    {
        timer.cancel();
        for (auto& [id, route] : routes)
            route->disconnect();
        peer = 0;
        hello_pending = true;
        acknowledgement_pending = false;
        // Change the destination token too: delayed old requests cannot enter a new session.
        session += SESSION_STRIDE;
        if (!session)
            ++session;
        transition(reason);
    }
    bool Core::send(Frame frame, Lane lane)
    {
        if (frame.kind != Kind::Hello && !peer)
            return false;
        if (frame.kind != Kind::Hello && frame.kind != Kind::HelloAck && state.load() != LinkState::Connected)
            return false;
        frame.source = session;
        frame.destination = frame.kind == Kind::Hello ? 0 : peer;
        const auto result = transport->trySend(pack(frame), lane);
        return result == SendResult::Sent;
    }
    void Core::receive(const Frame& frame)
    {
        if (frame.kind == Kind::Hello) {
            if (frame.endpoint || frame.id || !frame.payload.empty() || frame.destination)
                throw std::invalid_argument("invalid p2p hello");
            if (peer && peer != frame.source && state.load() == LinkState::Connected)
                fail(LinkState::Disconnected);
            if (peer != frame.source) {
                peer = frame.source;
                hello_pending = true;
            }
            acknowledgement_pending = true;
            last_received = Clock::now();
            return;
        }
        if (frame.destination != session || frame.source != peer)
            return; // Old session, never replay.
        last_received = Clock::now();
        if (frame.kind == Kind::HelloAck) {
            if (frame.endpoint || frame.id || !frame.payload.empty())
                throw std::invalid_argument("invalid p2p hello acknowledgement");
            transition(LinkState::Connected);
            return;
        }
        if (state.load() != LinkState::Connected)
            return;
        if (frame.kind == Kind::Heartbeat) {
            if (frame.endpoint || frame.id || frame.code || !frame.payload.empty())
                throw std::invalid_argument("invalid p2p heartbeat");
            return;
        }
        auto it = routes.find(frame.endpoint);
        if (it == routes.end())
            throw std::invalid_argument("unregistered p2p endpoint");
        it->second->receive(frame);
    }
    void Core::receiveFrames()
    {
        Bytes bytes;
        // Bound one dispatch so peer traffic cannot monopolize the executor.
        unsigned received{};
        for (; received < MAX_DISPATCH_FRAMES && transport->tryReceive(bytes); ++received)
            receive(unpack(bytes));
        if (received == MAX_DISPATCH_FRAMES)
            wake();
    }
    void Core::updateLink()
    {
        const auto now = Clock::now();
        if (peer && now - last_received >= options.link_timeout)
            fail(LinkState::TimedOut);
        if (now >= next_heartbeat) {
            if (!peer || state.load() != LinkState::Connected)
                hello_pending = true;
            else
                send(Frame{ .kind = Kind::Heartbeat, .payload = {} }, Lane::Urgent);
            next_heartbeat = now + options.heartbeat;
        }
        if (hello_pending && send(Frame{ .kind = Kind::Hello, .payload = {} }, Lane::Urgent))
            hello_pending = false;
        if (acknowledgement_pending && !hello_pending &&
            send(Frame{ .kind = Kind::HelloAck, .payload = {} }, Lane::Urgent))
            acknowledgement_pending = false;
    }
    void Core::pumpRoutes()
    {
        // Rotate the first route: sustained RPC traffic must not starve
        // terminal action results or latest state on another normal route.
        if (!routes.empty()) {
            auto entry = routes.begin();
            std::advance(entry, route_cursor % routes.size());
            for (std::size_t i = 0; i < routes.size(); ++i) {
                entry->second->pump();
                if (++entry == routes.end())
                    entry = routes.begin();
            }
            route_cursor = (route_cursor + 1) % routes.size();
        }
    }
    void Core::scheduleNext()
    {
        auto deadline = next_heartbeat;
        for (auto& [id, route] : routes)
            deadline = std::min(deadline, route->deadline());
        // Expired jobs are retired by pump(); never spin on an expired deadline.
        deadline = std::max(deadline, Clock::now() + std::chrono::milliseconds(1));
        timer = context.scheduleAt(deadline, wakeup());
    }
    void Core::drain() noexcept
    {
        try {
            timer.cancel();
            if (closing.load()) {
                for (auto& [id, route] : routes)
                    route->disconnect();
                routes.clear();
                transport->setWakeup({});
                transition(LinkState::Closed);
                return;
            }
            if (!transport->connected()) {
                if (state.load() != LinkState::Disconnected)
                    fail(LinkState::Disconnected);
                return;
            }
            receiveFrames();
            updateLink();
            pumpRoutes();
            scheduleNext();
        } catch (...) {
            try {
                fail(LinkState::ProtocolError);
                next_heartbeat = Clock::now() + options.heartbeat;
                timer = context.scheduleAt(next_heartbeat, wakeup());
            } catch (...) {
                closing.store(true);
                try {
                    transition(LinkState::ProtocolError);
                } catch (...) {
                    state.store(LinkState::ProtocolError);
                }
            }
        }
    }
}
