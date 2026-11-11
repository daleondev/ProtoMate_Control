#include "Server.hpp"
#include "util/Storage.hpp"
#include "util/Memory.h"
#include "catalog/Runtime.hpp"
#include "ui/app/Services.hpp"
#include <chrono>
#include <thread>
#include "mp_printf.h"
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/accesscontrol_default.h>
#if defined(CORE_CM7)
#include "transport/NetX.h"
#include "network/Wifi.h"
#else
extern "C" UA_StatusCode opcua_posix_configure(UA_ServerConfig* config);
#endif

namespace ui::opcua
{
    namespace
    {
        struct Instance {
            UA_Server* server{};
            catalog::Runtime* model{};
            bool started{};
            void reset() noexcept {
                if (server) {
                    if (started) UA_Server_run_shutdown(server);
                    UA_Server_delete(server); server = nullptr;
                }
                if (model) {std::destroy_at(model); opcua_memory_release(model); model = nullptr;}
                started = false;
            }
            ~Instance() {reset();}
        };
        template<typename Item>
        UA_Boolean deny_address_space_change(UA_Server*, UA_AccessControl*, const UA_NodeId*, void*, const Item*)
        {
            return false;
        }
    }
    Server::Status Server::status() const { std::scoped_lock lock{ m_mutex }; return m_status; }
    void Server::enabled(bool value)
    {
        m_services.remotePermission(false);
        std::scoped_lock lock{m_mutex};
        m_enabled = value;
        ++m_generation;
    }
    void Server::restart() {enabled(true);}
    void Server::failed()
    {
        m_services.remotePermission(false);
        std::scoped_lock lock{m_mutex};
        m_enabled = false; m_status.running = false;
        m_status.error = UA_STATUSCODE_BADINTERNALERROR;
        m_status.used = opcua_memory_stats().used;
    }
    void Server::run(const std::stop_token& stop)
    {
        const auto storage{ server_storage() };
        if (!opcua_memory_install(storage.data(), storage.size())) {
            std::scoped_lock lock{ m_mutex }; m_status.error = UA_STATUSCODE_BADOUTOFMEMORY; return;
        }
        {
            std::scoped_lock lock{ m_mutex };
            m_status.storage_verified = true;
            m_status.capacity = storage.size();
        }
        UA_Logger logger{[](void* context, UA_LogLevel, UA_LogCategory, const char* format, va_list arguments) {
            auto& owner{*static_cast<Server*>(context)};
            std::array<char,192> text{};
            mp_vsnprintf(text.data(), text.size(), format, arguments);
            std::scoped_lock lock{owner.m_mutex};
            owner.m_status.message = text;
            ++owner.m_status.log_sequence;
        }, this, nullptr};
        Instance instance;
        auto& server{instance.server};
        auto& model{instance.model};
        const auto close{[&] {instance.reset();}};
        auto nextAttempt{ std::chrono::steady_clock::now() };
        auto nextPublication{ nextAttempt };
        std::uint64_t generation{};
        while (!stop.stop_requested()) {
            bool requested{};
            std::uint64_t nextGeneration{};
            {std::scoped_lock lock{m_mutex}; requested = m_enabled; nextGeneration = m_generation;}
            if (generation != nextGeneration) {close(); generation = nextGeneration; nextAttempt = std::chrono::steady_clock::now();}
            const bool connected{ requested && m_network.status().state == Network::State::Connected };
            if (server && !connected) close();
            if (!server && connected && std::chrono::steady_clock::now() >= nextAttempt) {
                const auto startupBegin{std::chrono::steady_clock::now()};
                UA_ServerConfig config{};
#if defined(CORE_CM7)
                auto error{ opcua_netx_configure(&config, wifi_ip(), wifi_packet_pool()) };
#else
                auto error{ opcua_posix_configure(&config) };
#endif
                config.logging = &logger;
                if (config.eventLoop) config.eventLoop->logger = &logger;
                if (error == UA_STATUSCODE_GOOD) error = UA_ServerConfig_setMinimalCustomBuffer(&config, 4840, nullptr, 8192, 8192);
                if (error == UA_STATUSCODE_GOOD) {
                    UA_String_clear(&config.applicationDescription.applicationUri);
                    config.applicationDescription.applicationUri = UA_STRING_ALLOC("urn:protomate:control:server");
                    UA_LocalizedText_clear(&config.applicationDescription.applicationName);
                    config.applicationDescription.applicationName = UA_LOCALIZEDTEXT_ALLOC("en", "ProtoMate control");
                    if (!config.applicationDescription.applicationUri.data || !config.applicationDescription.applicationName.text.data)
                        error = UA_STATUSCODE_BADOUTOFMEMORY;
                    for (std::size_t i{}; i < config.endpointsSize && !error; ++i) {
                        UA_ApplicationDescription_clear(&config.endpoints[i].server);
                        error = UA_ApplicationDescription_copy(&config.applicationDescription, &config.endpoints[i].server);
                    }
                }
                if (error == UA_STATUSCODE_GOOD) {
                    if (config.sessionPKI.clear) config.sessionPKI.clear(&config.sessionPKI);
                    config.sessionPKI = {};
                    error = UA_AccessControl_default(&config, true, nullptr, 0, nullptr);
                    config.accessControl.allowAddNode = deny_address_space_change<UA_AddNodesItem>;
                    config.accessControl.allowAddReference = deny_address_space_change<UA_AddReferencesItem>;
                    config.accessControl.allowDeleteNode = deny_address_space_change<UA_DeleteNodesItem>;
                    config.accessControl.allowDeleteReference = deny_address_space_change<UA_DeleteReferencesItem>;
                    config.maxSessions = 2; config.maxSecureChannels = 4;
                    config.maxSessionTimeout = 30000;
                    config.tcpMaxMsgSize = 16384; config.tcpMaxChunks = 2;
                    config.maxNodesPerRead = 32; config.maxNodesPerWrite = 16;
                    config.maxNodesPerBrowse = 16; config.maxNodesPerMethodCall = 4;
                    config.maxSubscriptions = 4; config.maxSubscriptionsPerSession = 2;
                    config.maxMonitoredItems = 64; config.maxMonitoredItemsPerSubscription = 32;
                    config.maxMonitoredItemsPerCall = 16;
                    config.queueSizeLimits = { 1, 1 };
                    config.samplingIntervalLimits.min = 50; config.publishingIntervalLimits.min = 50;
                    config.maxPublishReqPerSession = 4; config.maxNotificationsPerPublish = 32;
                    config.maxRetransmissionQueueSize = 4;
                }
                if (error == UA_STATUSCODE_GOOD) {
                    server = UA_Server_newWithConfig(&config);
                    if (!server) error = UA_STATUSCODE_BADOUTOFMEMORY;
                }
                if (server) {
                    try {
                    const auto ns{ UA_Server_addNamespace(server, "urn:protomate:control") };
                    auto* storage{ opcua_memory_allocate(sizeof(catalog::Runtime)) };
                    if (!storage) error = UA_STATUSCODE_BADOUTOFMEMORY;
                    else {
                        model = std::construct_at(static_cast<catalog::Runtime*>(storage), server, ns, m_services);
                        model->configureSessions();
                        error = model->install();
                    }
                    if (error == UA_STATUSCODE_GOOD) {error = UA_Server_run_startup(server); instance.started = error == UA_STATUSCODE_GOOD;}
                    } catch (const catalog::StatusError& failure) {error = failure.code;}
                      catch (const std::bad_alloc&) {error = UA_STATUSCODE_BADOUTOFMEMORY;}
                    if (error != UA_STATUSCODE_GOOD) close();
                } else UA_ServerConfig_clear(&config);
                { std::scoped_lock lock{ m_mutex }; m_status.error = error;
                    m_status.startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startupBegin).count(); }
                nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds{ 5 };
            }
            if (server) {
                if (std::chrono::steady_clock::now() >= nextPublication) {
                    services::Snapshot snapshot{};
                    m_services.read(snapshot);
                    model->update(snapshot);
                    model->model().system.remote_permission = m_services.remotePermission();
                    model->model().settings.level = m_services.graphics().brightness;
                    model->model().messages.revision = m_services.messageRevision();
                    const auto memory{opcua_memory_stats()};
                    model->model().diagnostics = {memory.capacity, memory.used, memory.peak, memory.failures};
                    nextPublication = std::chrono::steady_clock::now() + std::chrono::milliseconds{ 100 };
                }
                UA_Server_run_iterate(server, false);
            }
            const auto memory{ opcua_memory_stats() };
            {
                std::scoped_lock lock{ m_mutex };
                m_status.running = server != nullptr;
                m_status.clients = model ? model->sessions() : 0;
                m_status.used = memory.used; m_status.peak = memory.peak; m_status.failures = memory.failures;
                m_status.failed_size = memory.failed_size;
                m_status.largest_free_at_failure = memory.largest_free_at_failure;
                ++m_status.iterations;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
        }
        close();
        std::scoped_lock lock{ m_mutex };
        m_status.running = false; m_status.used = opcua_memory_stats().used;
    }
}
