#include "Bridge.hpp"

#include <stdexcept>
#include <utility>

// Startup and shutdown are outside the steady-state ICC transfer path. Keep
// them in ordinary flash on M4, leaving instruction RAM for dispatch and motion.
namespace icc
{
    Bridge::Bridge(pnm::msg::Bus& bus,
                   pnm::coro::Context& context,
                   ITransport& transport,
                   ConnectionOptions connection)
      : m_context{ context }
      , m_transport{ transport }
      , m_topics{ bus, m_outgoingWakeup }
      , m_services{ bus, context, m_outgoingWakeup, m_stopSource.get_token() }
      , m_actions{ bus, context, m_outgoingWakeup, m_stopSource.get_token() }
      , m_connection{ connection }
    {
    }

    Bridge::~Bridge() { stop(); }

    void Bridge::start()
    {
        std::unique_lock lock{ m_mutex };
        if (m_started) {
            throw std::logic_error("Bridge can only be started once");
        }
        m_started = true;

        try {
            // Construct all coroutine chains before scheduling any of them.
            // Moving these ready-made tasks into co_spawn also avoids allocating
            // a new loop task inside its detached entry callback.
            auto health{ runLoop(healthLoop(m_stopSource.get_token()), &m_healthRunning) };
            auto sender{ runLoop(sendLoop(Lane::Normal, m_stopSource.get_token()), &m_sendRunning) };
            auto receiver{ runLoop(receiveLoop(Lane::Normal, m_stopSource.get_token()), &m_receiveRunning) };

            auto urgent_sender{ runLoop(sendLoop(Lane::Urgent, m_stopSource.get_token()),
                                        &m_urgentSendRunning) };
            auto urgent_receiver{ runLoop(receiveLoop(Lane::Urgent, m_stopSource.get_token()),
                                          &m_urgentReceiveRunning) };
            auto background_sender{ runLoop(sendLoop(Lane::Background, m_stopSource.get_token()),
                                            &m_backgroundSendRunning) };
            auto background_receiver{ runLoop(receiveLoop(Lane::Background, m_stopSource.get_token()),
                                              &m_backgroundReceiveRunning) };

            pnm::coro::co_spawn(m_context, std::move(health));
            m_healthRunning = true;
            pnm::coro::co_spawn(m_context, std::move(urgent_sender));
            m_urgentSendRunning = true;
            pnm::coro::co_spawn(m_context, std::move(urgent_receiver));
            m_urgentReceiveRunning = true;
            pnm::coro::co_spawn(m_context, std::move(sender));
            m_sendRunning = true;

            pnm::coro::co_spawn(m_context, std::move(receiver));
            m_receiveRunning = true;
            pnm::coro::co_spawn(m_context, std::move(background_sender));
            m_backgroundSendRunning = true;
            pnm::coro::co_spawn(m_context, std::move(background_receiver));
            m_backgroundReceiveRunning = true;
            // Holding the mutex prevents an immediately completing handler from
            // clearing its flag before its scheduling has been recorded.
        } catch (...) {
            m_failure = std::current_exception();
            m_failureReason = ConnectionReason::InternalError;
            lock.unlock();
            // If only the first handler was scheduled, it must still finish.
            // Keep driving the context and call stop() before destruction.
            m_stopSource.request_stop();
            throw;
        }
    }

    void Bridge::stop(std::chrono::milliseconds grace)
    {
        {
            std::scoped_lock lock{ m_mutex };
            if (!m_started) {
                return;
            }
        }

        m_shutdownRequested.store(true);
        m_outgoingWakeup->notify();
        m_healthWakeup->notify();
        std::unique_lock lock{ m_mutex };
        const auto stopped{ [this] { return loopsStopped(); } };
        if (!m_finished.wait_for(lock, grace, stopped)) {
            // The grace period bounds action draining, not an already-running
            // final notification. Once cleanup starts, wait for that callback.
            if (!m_failure && !m_cleanupRunning) {
                m_failure = std::make_exception_ptr(
                  std::runtime_error("Bridge shutdown timed out; action completion is unknown"));
                m_failureReason = ConnectionReason::ShutdownTimeout;
            }
            lock.unlock();
            m_stopSource.request_stop();
            lock.lock();
            m_finished.wait(lock, stopped);
        }
        m_topics.close();
        m_outgoingWakeup->close();
        m_healthWakeup->ready.close();
    }
}
