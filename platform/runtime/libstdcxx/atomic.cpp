#include <cstddef>
#include <cstdint>

#if defined(HAL_PLATFORM_STM32)

#include "hal/stm32/InterruptGuard.hpp"

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

namespace
{
    [[nodiscard]] std::uint64_t read_value(const volatile void* pointer) noexcept
    {
        const auto* bytes{ static_cast<const volatile std::uint8_t*>(pointer) };
        std::uint64_t value{};
        for (std::size_t index{}; index < sizeof(value); ++index) {
            value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
        }
        return value;
    }

    void write_value(volatile void* pointer, std::uint64_t value) noexcept
    {
        auto* bytes{ static_cast<volatile std::uint8_t*>(pointer) };
        for (std::size_t index{}; index < sizeof(value); ++index) {
            bytes[index] = static_cast<std::uint8_t>(value >> (index * 8U));
        }
    }

    void copy_bytes(volatile void* destination, const volatile void* source, std::size_t size) noexcept
    {
        auto* destination_bytes{ static_cast<volatile std::uint8_t*>(destination) };
        const auto* source_bytes{ static_cast<const volatile std::uint8_t*>(source) };
        for (std::size_t index{}; index < size; ++index) {
            destination_bytes[index] = source_bytes[index];
        }
    }

    [[nodiscard]] bool equal_bytes(const volatile void* left,
                                   const volatile void* right,
                                   std::size_t size) noexcept
    {
        const auto* left_bytes{ static_cast<const volatile std::uint8_t*>(left) };
        const auto* right_bytes{ static_cast<const volatile std::uint8_t*>(right) };
        for (std::size_t index{}; index < size; ++index) {
            if (left_bytes[index] != right_bytes[index]) {
                return false;
            }
        }
        return true;
    }

    template<typename Operation>
    [[nodiscard]] std::uint64_t fetch_update(volatile void* pointer,
                                             std::uint64_t operand,
                                             Operation operation) noexcept
    {
        const hal::stm32::InterruptGuard interrupt_guard;
        const std::uint64_t previous{ read_value(pointer) };
        write_value(pointer, operation(previous, operand));
        return previous;
    }
}

// GCC emits these libatomic ABI calls for 64-bit std::atomic operations on
// Armv7-M. A short interrupt-masked region provides atomicity on this
// single-core target and remains usable before the ThreadX scheduler starts.
// NOLINTBEGIN(bugprone-reserved-identifier,cppcoreguidelines-macro-usage,readability-identifier-naming)
extern "C" std::uint64_t __atomic_load_8(const volatile void* pointer, int)
{
    const hal::stm32::InterruptGuard interrupt_guard;
    return read_value(pointer);
}

extern "C" void __atomic_store_8(volatile void* pointer, std::uint64_t value, int)
{
    const hal::stm32::InterruptGuard interrupt_guard;
    write_value(pointer, value);
}

extern "C" std::uint64_t __atomic_exchange_8(volatile void* pointer, std::uint64_t value, int)
{
    const hal::stm32::InterruptGuard interrupt_guard;
    const std::uint64_t previous{ read_value(pointer) };
    write_value(pointer, value);
    return previous;
}

extern "C" bool runtime_atomic_compare_exchange_8(volatile void*, void*, std::uint64_t, int, int) asm(
  "__atomic_compare_exchange_8");

extern "C" bool runtime_atomic_compare_exchange_8(volatile void* pointer,
                                                  void* expected_pointer,
                                                  std::uint64_t desired,
                                                  int,
                                                  int)
{
    const hal::stm32::InterruptGuard interrupt_guard;
    const std::uint64_t current{ read_value(pointer) };
    auto* expected{ static_cast<std::uint64_t*>(expected_pointer) };
    if (current == *expected) {
        write_value(pointer, desired);
        return true;
    }
    *expected = current;
    return false;
}

#define RUNTIME_DEFINE_ATOMIC_FETCH_OPERATION(name, expression)                                              \
    extern "C" std::uint64_t __atomic_fetch_##name##_8(volatile void* pointer, std::uint64_t value, int)     \
    {                                                                                                        \
        return fetch_update(                                                                                 \
          pointer, value, [](std::uint64_t left, std::uint64_t right) { return expression; });               \
    }                                                                                                        \
    extern "C" std::uint64_t __atomic_##name##_fetch_8(volatile void* pointer, std::uint64_t value, int)     \
    {                                                                                                        \
        const std::uint64_t previous{ __atomic_fetch_##name##_8(pointer, value, __ATOMIC_SEQ_CST) };         \
        const std::uint64_t left{ previous };                                                                \
        const std::uint64_t right{ value };                                                                  \
        return expression;                                                                                   \
    }

RUNTIME_DEFINE_ATOMIC_FETCH_OPERATION(add, left + right)
RUNTIME_DEFINE_ATOMIC_FETCH_OPERATION(sub, left - right)
RUNTIME_DEFINE_ATOMIC_FETCH_OPERATION(and, left& right)
RUNTIME_DEFINE_ATOMIC_FETCH_OPERATION(or, left | right)
RUNTIME_DEFINE_ATOMIC_FETCH_OPERATION(xor, left ^ right)
RUNTIME_DEFINE_ATOMIC_FETCH_OPERATION(nand, ~(left& right))

#undef RUNTIME_DEFINE_ATOMIC_FETCH_OPERATION

extern "C" bool __atomic_is_lock_free(std::size_t size, const volatile void* pointer)
{
    // Match the native exclusive-access widths and their alignment. Other
    // sizes (including three-byte records) use the generic masked helpers.
    return size == 1U ||
           ((size == 2U || size == 4U) && reinterpret_cast<std::uintptr_t>(pointer) % size == 0U);
}

// Generic libatomic ABI used for std::atomic<T> when T has no sized compiler
// intrinsic. The asm labels avoid conflicting with GCC's builtin spellings.
extern "C" void runtime_atomic_load(std::size_t, void*, void*, int) asm("__atomic_load");
extern "C" void runtime_atomic_store(std::size_t, void*, void*, int) asm("__atomic_store");
extern "C" void runtime_atomic_exchange(std::size_t, void*, void*, void*, int) asm("__atomic_exchange");
extern "C" bool runtime_atomic_compare_exchange(std::size_t, void*, void*, void*, int, int) asm(
  "__atomic_compare_exchange");

extern "C" void runtime_atomic_load(std::size_t size, void* object, void* result, int)
{
    const hal::stm32::InterruptGuard interrupt_guard;
    copy_bytes(result, object, size);
}

extern "C" void runtime_atomic_store(std::size_t size, void* object, void* value, int)
{
    const hal::stm32::InterruptGuard interrupt_guard;
    copy_bytes(object, value, size);
}

extern "C" void runtime_atomic_exchange(std::size_t size, void* object, void* value, void* result, int)
{
    const hal::stm32::InterruptGuard interrupt_guard;
    auto* object_bytes{ static_cast<volatile std::uint8_t*>(object) };
    auto* value_bytes{ static_cast<volatile std::uint8_t*>(value) };
    auto* result_bytes{ static_cast<volatile std::uint8_t*>(result) };
    for (std::size_t index{}; index < size; ++index) {
        const std::uint8_t previous{ object_bytes[index] };
        object_bytes[index] = value_bytes[index];
        result_bytes[index] = previous;
    }
}

extern "C" bool runtime_atomic_compare_exchange(std::size_t size,
                                                void* object,
                                                void* expected,
                                                void* desired,
                                                int,
                                                int)
{
    const hal::stm32::InterruptGuard interrupt_guard;
    if (equal_bytes(object, expected, size)) {
        copy_bytes(object, desired, size);
        return true;
    }
    copy_bytes(expected, object, size);
    return false;
}
// NOLINTEND(bugprone-reserved-identifier,cppcoreguidelines-macro-usage,readability-identifier-naming)

#endif
