#pragma once
#include "Network.hpp"
#include <cstddef>
#include <array>
#include <cstdint>
#include <mutex>
#include <stop_token>
namespace ui { class Services; }

namespace ui::opcua
{
    class Server final
    {
      public:
        struct Status
        {
            bool running{}, storage_verified{};
            std::array<char,192> message{};
            std::uint64_t log_sequence{};
            std::uint32_t error{};
            unsigned clients{};
            std::size_t used{}, peak{}, capacity{}, failures{};
            std::size_t failed_size{}, largest_free_at_failure{};
            std::uint64_t iterations{}, startup_ms{};
        };
        Server(Network& network, Services& services) : m_network{ network }, m_services{ services } {}
        void run(const std::stop_token& stop);
        Status status() const;
        void enabled(bool value);
        void restart();
        void failed();
      private:
        Network& m_network;
        Services& m_services;
        mutable std::mutex m_mutex;
        Status m_status;
        bool m_enabled{true};
        std::uint64_t m_generation{};
    };
}
