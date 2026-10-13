#pragma once
#include <cstdint>
inline constexpr unsigned USBD_OK=0, USBD_BUSY=1;
unsigned CDC_Transmit_FS(std::uint8_t*, std::uint16_t);
