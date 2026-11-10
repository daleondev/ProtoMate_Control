#pragma once
#include "simulation/Protocol.hpp"
#include <array>
#include <filesystem>
#include <span>
namespace simulation
{
    class Media final
    {
      public:
        Media(Media&&) = delete;
        auto operator=(Media&&) -> Media& = delete;

        explicit Media(std::filesystem::path directory = {});
        ~Media();
        Media(const Media&) = delete;
        Media& operator=(const Media&) = delete;
        void execute(const Packet& q, Packet& reply);

      private:
        std::filesystem::path m_directory;
        std::array<int, 2> m_fd{ -1, -1 };
        std::array<bool, 2> m_created{};
        void open(unsigned device);
        void write(const Packet& q, Packet& r);
    };
}
