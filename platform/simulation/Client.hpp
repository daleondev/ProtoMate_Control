#pragma once
#include "SharedMemory.hpp"
#include "hal/drivers/util/StepHardware.hpp"
#include <functional>
#include <optional>
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
    Packet command(Operation operation, std::array<std::uint64_t, ARGUMENT_COUNT> arguments = {}) noexcept;
    bool publish_bank(unsigned axis,
                      unsigned bank,
                      std::span<const std::byte> bytes,
                      std::optional<std::uint32_t> cut = {}) noexcept;
    hal::util::StepSample sample() noexcept;
    std::uint64_t now() noexcept;
    void set_step_callback(std::function<void()> callback);
    void dispatch_inputs() noexcept;
    void register_timer(unsigned index, void* object, void (*callback)(void*)) noexcept;
    void dispatch_timers() noexcept;
    void register_input(unsigned pin, hal::GpioInput* input) noexcept;
    bool in_interrupt() noexcept;
    void enter() noexcept;
    void leave() noexcept;
    class Transaction
    {
      public:
        Transaction(Transaction&&) = delete;
        auto operator=(Transaction&&) -> Transaction& = delete;

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
