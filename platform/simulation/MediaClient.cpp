#include "MediaClient.hpp"
#include "Client.hpp"
#include <algorithm>
namespace
{
    bool transfer(simulation::Operation op, unsigned device, std::size_t offset, void* data, std::size_t size)
    {
        auto* bytes{ static_cast<std::byte*>(data) };
        while (size) {
            const auto count{ std::min(size, simulation::PAYLOAD_BYTES) };
            simulation::client::Transaction transaction{};
            simulation::Packet q{ .operation = op, .arguments = { device, offset, count } };
            if (op == simulation::Operation::MediaWrite) {
                std::copy_n(bytes, count, q.data.begin());
                q.size = static_cast<std::uint32_t>(count);
            }
            auto reply{ simulation::client::call(q) };
            if (reply.status)
                return false;
            if (op == simulation::Operation::MediaRead) {
                if (reply.size != count)
                    return false;
                std::copy_n(reply.data.begin(), count, bytes);
            }
            if (bytes)
                bytes += count;
            offset += count;
            size -= count;
        }
        return true;
    }
}
extern "C" bool protomate_sim_media_info(unsigned device, bool* created)
{
    simulation::client::Transaction transaction{};
    auto r{ simulation::client::command(simulation::Operation::MediaInfo, { device }) };
    if (r.status)
        return false;
    *created = r.arguments[1] != 0;
    return true;
}
extern "C" bool protomate_sim_media_read(unsigned device, std::size_t offset, void* data, std::size_t size)
{
    return transfer(simulation::Operation::MediaRead, device, offset, data, size);
}
extern "C" bool protomate_sim_media_write(unsigned device,
                                          std::size_t offset,
                                          const void* data,
                                          std::size_t size)
{
    return transfer(simulation::Operation::MediaWrite, device, offset, const_cast<void*>(data), size);
}
extern "C" bool protomate_sim_media_erase(std::size_t offset, std::size_t size)
{
    return transfer(simulation::Operation::MediaErase, 0, offset, nullptr, size);
}

extern "C" bool protomate_sim_media_flush(unsigned device)
{
    simulation::client::Transaction transaction{};
    return simulation::client::command(simulation::Operation::MediaFlush, { device }).status == 0;
}
