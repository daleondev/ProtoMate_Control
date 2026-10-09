#include "hal/board/board.hpp"
#include "soes_port.hpp"
extern "C" {
#include "ecat_slv.h"
extern uint32_t command_value, echo_value;
}
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <system_error>
#include <thread>

int main()
{
    using namespace std::chrono_literals;
    try {
        auto select = hal::board::createEthercatChipSelect();
        auto spi = hal::board::createEthercatSpi();
        if (!select || !spi) throw std::runtime_error("SPI5/CS unavailable");
        hal::device::Lan9253 esc{*spi, *select};
        ethercat_echo::attach(esc);
        esc_cfg_t config{};
        config.use_interrupt = 0;
        config.watchdog_cnt = 150;
        config.safeoutput_override = ethercat_echo::safeOutputs;
        std::printf("[ethercat] ProtoMate Echo starting; J17=000; no robot hardware required\n");
        ecat_slv_init(&config);
        uint16_t previous = 0xffff;
        auto report = std::chrono::steady_clock::now();
        for (;;) {
            ecat_slv();
            const auto now = std::chrono::steady_clock::now();
            if (ESCvar.ALstatus != previous || now >= report) {
                const auto link = esc.readEscRegister(0x110, 2);
                if (!link) throw std::system_error(link.error(), "ESC link status");
                std::printf("[ethercat] AL=%02x error=%04x DL=%04lx command=%lu echo=%lu\n",
                    ESCvar.ALstatus, ESCvar.ALerror, static_cast<unsigned long>(*link),
                    static_cast<unsigned long>(command_value), static_cast<unsigned long>(echo_value));
                std::fflush(stdout);
                previous = ESCvar.ALstatus;
                report = now + 5s;
            }
            std::this_thread::sleep_for(1ms);
        }
    } catch (const std::exception& error) {
        ethercat_echo::safeOutputs();
        std::printf("[ethercat] STOPPED: %s. Check wiring/power and reset the Nucleo.\n", error.what());
        std::fflush(stdout);
        for (;;) std::this_thread::sleep_for(1s);
    }
}
