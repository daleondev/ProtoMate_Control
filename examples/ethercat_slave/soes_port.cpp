#include "soes_port.hpp"
extern "C" {
#include "ecat_slv.h"
#include "device.h"
extern uint32_t command_value, echo_value;
}
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    hal::device::Lan9253* device{};

    template<typename T>
    T require(hal::util::Result<T> result)
    {
        if (!result) throw std::system_error(result.error(), "LAN9253 transfer");
        if constexpr (!std::is_void_v<T>) return *result;
    }

    // LAN9253 supports both registers and process RAM through the same CSR
    // interface. Small aligned chunks keep this proof of concept simple.
    void transfer(uint16_t address, uint8_t* data, uint16_t length, bool write)
    {
        if (std::uint32_t{address} + length > 0x3000 || (length && !data))
            throw std::invalid_argument("ESC memory range");
        const auto deadline = Clock::now() + 100ms;
        while (length) {
            const uint8_t size = (address % 4 == 0 && length >= 4) ? 4 :
                                 (address % 2 == 0 && length >= 2) ? 2 : 1;
            const auto timeout = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            if (timeout <= 0ms) throw std::system_error(std::make_error_code(std::errc::timed_out));
            uint32_t value{};
            if (write) {
                for (unsigned i = 0; i < size; ++i) value |= std::uint32_t{data[i]} << (8 * i);
                require(device->writeEscRegister(address, size, value, timeout));
            } else {
                value = require(device->readEscRegister(address, size, timeout));
                for (unsigned i = 0; i < size; ++i) data[i] = static_cast<uint8_t>(value >> (8 * i));
            }
            address += size; data += size; length -= size;
        }
    }

    void refreshEvents()
    {
        // SOES expects the current AL event register after every ESC access.
        ESCvar.ALevent = require(device->readEscRegister(ESCREG_ALEVENT, 4));
    }

    void serviceEeprom()
    {
        if (!(ESCvar.ALevent & ESCREG_ALEVENT_EEP)) return;
        const auto status = require(device->readEscRegister(0x502, 2));
        if (!(status & 0x8000)) return;
        if (!(status & 0x20)) throw std::runtime_error("J17 must select EEPROM emulation");
        const auto command = status & 0x700;
        const auto word = require(device->readEscRegister(0x504, 4));
        uint16_t acknowledge = command;
        // LAN9253 RELOAD is 4 (not the generic SOES EEP_CMD_RELOAD value 3).
        // Serve one request per stack iteration, with bounded bus operations.
        if ((command == 0x100 || command == 0x400) && word < EC_SII_SIZE / 2) {
            std::array<uint8_t, 8> data;
            data.fill(0xff);
            // The ESC fetches eight bytes even when the master requests just
            // the final word(s). Pad the tail without reading past the image.
            std::copy_n(ec_sii_image + word * 2,
                std::min<std::size_t>(data.size(), EC_SII_SIZE - word * 2), data.begin());
            transfer(0x508, data.data(), data.size(), true);
        } else {
            acknowledge |= 0x2000; // Unsupported write/address: acknowledge error.
        }
        require(device->writeEscRegister(0x502, 2, acknowledge));
        refreshEvents();
    }
}

namespace ethercat_echo
{
    void attach(hal::device::Lan9253& esc) { device = &esc; }
    void safeOutputs() { command_value = 0; echo_value = 0; }
}

extern "C" void ESC_read(uint16_t address, void* buffer, uint16_t length)
{
    transfer(address, static_cast<uint8_t*>(buffer), length, false);
    refreshEvents();
}

extern "C" void ESC_write(uint16_t address, void* buffer, uint16_t length)
{
    transfer(address, static_cast<uint8_t*>(buffer), length, true);
    refreshEvents();
}

extern "C" void ESC_reset(void) {}

extern "C" void ESC_init(const esc_cfg_t*)
{
    if (!device) throw std::runtime_error("ESC transport missing");
    const auto deadline = Clock::now() + 2s;
    while (require(device->readSystemRegister(hal::device::Lan9253::byteTestAddress)) !=
           hal::device::Lan9253::byteTestValue) {
        if (Clock::now() >= deadline) throw std::runtime_error("ESC SPI startup timeout");
        std::this_thread::sleep_for(1ms);
    }
    // An independently powered ESC may retain the previous program's setup.
    // Reset only the EtherCAT core, and reload this image's boot configuration.
    if (require(device->readSystemRegister(hal::device::Lan9253::hardwareConfigAddress)) &
        hal::device::Lan9253::readyMask) {
        require(device->writeSystemRegister(0x1f8, 0x40));
        std::this_thread::sleep_for(2ms);
    }
    const auto identity = require(device->initializeEmulatedBoot(std::span{ec_sii_image}.first<16>()));
    std::printf("[ethercat] ESC %08lx; emulated SII ready, SPI %u Hz\n",
                static_cast<unsigned long>(identity.id_revision), 937500U);
    // Bound the condition SOES otherwise waits for indefinitely in init.
    const auto dlDeadline = Clock::now() + 250ms;
    while (!(require(device->readEscRegister(0x110, 2)) & 1)) {
        if (Clock::now() >= dlDeadline) throw std::runtime_error("ESC EEPROM load timeout");
        std::this_thread::sleep_for(1ms);
    }
    ESCvar.esc_hw_eep_handler = serviceEeprom;
    refreshEvents();
}

extern "C" void cb_get_inputs(void) {}
extern "C" void cb_set_outputs(void) { echo_value = command_value; }
