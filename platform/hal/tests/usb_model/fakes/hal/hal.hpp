#pragma once
#include <cstdint>
#include <cstdlib>
inline std::uint32_t usbTestPrimask{};
inline auto __get_PRIMASK() { return usbTestPrimask; }
inline void __disable_irq() { usbTestPrimask = 1; }
inline void __set_PRIMASK(std::uint32_t value) { usbTestPrimask = value; }
inline void __DMB() {}
struct UsbTestScb { std::uint32_t ICSR{}; };
inline UsbTestScb usbTestScb;
inline auto* SCB = &usbTestScb;
inline constexpr std::uint32_t SCB_ICSR_PENDSVSET_Msk = 1u << 28;
[[noreturn]] inline void Error_Handler() { std::abort(); }
