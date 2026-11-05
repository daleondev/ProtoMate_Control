#pragma once
#include <optional>
#include "SharedMemory.hpp"
#include "hal/drivers/util/StepHardware.hpp"
#include <functional>
namespace hal
{
    class GpioInput;
}

namespace simulation::client
{
    bool connected() noexcept;
    bool failed() noexcept;
    SharedMemory* memory() noexcept;
    Packet call(Packet request) noexcept;
    Packet command(Operation operation, std::array<std::uint64_t, 8> arguments = {}) noexcept;
    bool publishBank(unsigned axis, unsigned bank, std::span<const std::byte> bytes, std::optional<std::uint32_t> cut = {}) noexcept;
    hal::util::StepSample sample() noexcept;
    std::uint64_t now() noexcept;
    void setStepCallback(std::function<void()> callback);
    void dispatchInputs() noexcept;
    void registerTimer(unsigned index, void* object, void (*callback)(void*)) noexcept;
    void dispatchTimers() noexcept;
    void registerInput(unsigned pin, hal::GpioInput* input) noexcept;
    bool inInterrupt() noexcept;
    void enter() noexcept;
    void leave() noexcept;
    class Transaction
    {
      public:
        Transaction() noexcept;
        ~Transaction();
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

      private:
        unsigned m_posture{};
        bool m_active{};
    };
}
extern "C" int protomate_sim_wait_interrupt();
extern "C" void protomate_sim_dispatch_interrupt();
extern "C" void protomate_sim_ack_interrupt();
