#pragma once
#include "util/Core.hpp"

namespace p2p
{
    // Register routes before start(). Context has exactly one application-owned
    // runner. close() schedules teardown; keep the bus/context alive until Closed.
    class Bridge final
    {
      public:
        Bridge(pnm::msg::Bus& bus,
               pnm::coro::Context& context,
               std::shared_ptr<ITransport> transport,
               Options options = {})
          : m_core(std::make_shared<util::Core>(bus, context, std::move(transport), options))
        {
        }
        ~Bridge() { m_core->close(); }
        Bridge(const Bridge&) = delete;
        Bridge& operator=(const Bridge&) = delete;
        void start() { m_core->start(); }
        void close() { m_core->close(); }
        LinkState state() const { return m_core->state.load(); }
        void onStateChanged(std::function<void(LinkState)> callback)
        {
            if (m_core->started.load())
                throw std::logic_error("register p2p state handler before start");
            m_core->changed = std::move(callback);
        }
        template<typename T>
        void publishTopic(EndpointId id, std::string_view name);
        template<typename T>
        void receiveTopic(EndpointId id, std::string_view name);
        template<typename Q, typename R>
        void importService(EndpointId id, std::string_view name, Lane lane = Lane::Normal);
        template<typename Q, typename R>
        void exportService(EndpointId id, std::string_view name, Lane lane = Lane::Normal);
        template<typename G, typename F, typename R>
        void importAction(EndpointId id, std::string_view name);
        template<typename G, typename F, typename R>
        void exportAction(EndpointId id, std::string_view name);

      private:
        std::shared_ptr<util::Core> m_core;
    };
}
#include "util/Routes.hpp"
