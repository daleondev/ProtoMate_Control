#pragma once
#include "simulation/Protocol.hpp"
#include <pneumo/messaging.hpp>

namespace pnm::utils::memory
{
    template<>
    struct SerializationAdapter<simulation::Packet>
    {
        static std::size_t bufferSize(const simulation::Packet&) { return simulation::PACKET_BYTES; }
        static bool serialize(const simulation::Packet& value, std::span<std::byte> output)
        {
            if (output.size() != simulation::PACKET_BYTES)
                return false;
            const auto bytes{ simulation::encode(value) };
            std::copy(bytes.begin(), bytes.end(), output.begin());
            return true;
        }
        static bool deserialize(std::span<const std::byte> input, simulation::Packet& value)
        {
            try {
                value = simulation::decode(input);
                return true;
            } catch (...) {
                return false;
            }
        }
    };
}

namespace simulation
{
    enum class Provider
    {
        Machine,
        Controller
    };
    struct Catalog
    {
        [[= Provider::Machine]] pnm::msg::Service<Packet, Packet> control;
        pnm::msg::Topic<PlantSnapshot> state;
    };
}
