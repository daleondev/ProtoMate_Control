#include "hal/board/board.hpp"
#include "hal/devices/impl/Lan9253.hpp"
#include "runtime/thread.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

extern "C" {
volatile std::uint32_t hardware_spi_test_status{};
volatile std::uint32_t hardware_spi_test_completed{};
[[gnu::noinline, gnu::used]] void hardware_spi_test_complete() { asm volatile("" ::: "memory"); }
}

namespace
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using Esc = hal::device::Lan9253;
    std::mutex log_mutex;

    template<typename... Args>
    void log(const char* format, Args... args)
    {
        const std::scoped_lock lock{ log_mutex };
        std::printf("[spi-test] ");
        std::printf(format, args...);
        std::printf("\n");
        std::fflush(stdout);
    }

    template<typename T>
    T require(hal::util::Result<T> result, const char* operation)
    {
        if (!result) {
            const auto message = result.error() == std::errc::resource_unavailable_try_again ?
                "ESC READY=0 (configuration missing or reset)" : result.error().message();
            throw std::runtime_error(std::string{ operation } + ": " + message);
        }
        if constexpr (!std::is_void_v<T>) return *result;
    }

    enum class Operation { Loopback, Probe, Boot, Stress };

    class Bench
    {
      public:
        Bench()
        {
            m_select = hal::board::createEthercatChipSelect();
            if (!m_select) throw std::runtime_error("PF6 chip select unavailable");
            m_bus = hal::board::createEthercatSpi();
            if (!m_bus) throw std::runtime_error("SPI5 unavailable");
            m_esc = std::make_unique<Esc>(*m_bus, *m_select);
            if (m_bus->clockFrequencyHz() != 937500U)
                throw std::runtime_error("SPI5 clock mismatch; expected 937500 Hz");
            log("READY: SPI5 mode 0, 8-bit MSB-first, clock=%lu Hz; CS high",
                static_cast<unsigned long>(m_bus->clockFrequencyHz()));
            log("No automatic transfers. EN_N stays high; motor timers/UART and storage are not started.");
        }

        ~Bench()
        {
            m_stop.store(true);
            if (m_worker.joinable()) m_worker.join();
            m_select->write(hal::gpio::Level::High);
        }

        static void help()
        {
            log("help | status | loopback | probe | boot | stress [count=100000] | stop");
            log("loopback: disconnect EVB SPI, bridge CN9.28 MOSI to CN9.24 MISO. CS stays high.");
            log("probe/stress: REMOVE loopback jumper; connect EVB SPI with J17=000 (both EMUL0 jumpers).");
            log("stress checks BYTE_TEST, READY and stable chip ID/revision; fails on the first bad read.");
            log("boot: supply volatile emulated boot configuration with J17=000; no flash/EEPROM programming.");
        }

        void command(std::string_view line)
        {
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
            if (line.empty()) return;
            if (line == "help") return help();
            if (line == "status") {
                log("%s; completed=%lu; clock=%lu Hz; last BYTE_TEST=%08lx HW_CFG=%08lx ID_REV=%08lx",
                    m_busy.load() ? "BUSY" : "IDLE", static_cast<unsigned long>(m_completed.load()),
                    static_cast<unsigned long>(m_bus->clockFrequencyHz()),
                    static_cast<unsigned long>(m_byteTest.load()), static_cast<unsigned long>(m_config.load()),
                    static_cast<unsigned long>(m_id.load()));
                return;
            }
            if (line == "stop") {
                m_stop.store(true);
                log("Stop requested; worker releases CS before returning to READY.");
                return;
            }
            Operation operation{};
            std::uint32_t count = 100000;
            if (line == "loopback") operation = Operation::Loopback;
            else if (line == "probe") operation = Operation::Probe;
            else if (line == "boot") operation = Operation::Boot;
            else if (line == "stress" || line.starts_with("stress ")) {
                operation = Operation::Stress;
                if (line.size() > 6) {
                    const auto text = line.substr(7);
                    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
                    if (error != std::errc{} || end != text.data() + text.size() || count == 0 || count > 1000000) {
                        log("REJECTED: stress count must be 1..1000000");
                        return;
                    }
                }
            }
            else {
                log("Unknown command. Type help.");
                return;
            }
            if (m_busy.load()) {
                log("BUSY: use status or stop");
                return;
            }
            if (m_worker.joinable()) m_worker.join();
            m_stop.store(false);
            m_completed.store(0);
            m_busy.store(true);
            m_worker = std::jthread{ [this, operation, count] { run(operation, count); } };
        }

      private:
        void cancelled() const
        {
            if (m_stop.load()) throw std::runtime_error("cancelled");
        }

        void completed(std::uint32_t count)
        {
            m_completed.store(count);
            hardware_spi_test_completed = count;
        }

        Esc::Identity probe()
        {
            const auto deadline = Clock::now() + 2s;
            std::error_code last_error;
            do {
                cancelled();
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
                if (remaining <= 0ms) break;
                const auto identity = m_esc->identify(std::min(100ms, remaining));
                if (identity) {
                    remember(*identity);
                    log("BYTE_TEST=0x%08lx READY=1 ID_REV=0x%08lx (chip 0x9253)",
                        static_cast<unsigned long>(identity->byte_test),
                        static_cast<unsigned long>(identity->id_revision));
                    return *identity;
                }
                last_error = identity.error();
                std::this_thread::sleep_for(1ms);
            } while (Clock::now() < deadline);
            // These two registers are safe to read even before READY.
            const auto byte = m_esc->readSystemRegister(Esc::byteTestAddress);
            const auto config = m_esc->readSystemRegister(Esc::hardwareConfigAddress);
            if (byte) m_byteTest.store(*byte);
            if (config) m_config.store(*config);
            log("Probe: BYTE_TEST=%08lx HW_CFG=%08lx",
                static_cast<unsigned long>(m_byteTest.load()), static_cast<unsigned long>(m_config.load()));
            if (byte && *byte == Esc::byteTestValue && config && (*config & Esc::readyMask) == 0) {
                log("SPI responds correctly; ESC READY=0. J17=000 requires emulated EEPROM boot data.");
                log("Run boot, then probe/stress. A blank SAM supplies no boot data by itself.");
                throw std::runtime_error("ESC configuration is not loaded; SPI byte test passed");
            }
            throw std::runtime_error("ESC not identified within 2 s: " + last_error.message());
        }

        void boot()
        {
            // Minimal test-only configuration: ordinary SPI 0x80, GPIO inputs,
            // no device emulation, no SYNC output or network setup. Byte 14 is
            // the SII CRC-8 (init FF, polynomial 07) over bytes 0..13.
            static constexpr std::array<std::uint8_t, 16> configuration{
                0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xe9, 0 };
            log("Serving 16-byte volatile boot configuration; SAM flash and physical EEPROM are untouched.");
            const auto identity = require(m_esc->initializeEmulatedBoot(configuration), "emulated boot");
            remember(identity);
            completed(1);
            log("BYTE_TEST=0x%08lx READY=1 ID_REV=0x%08lx", static_cast<unsigned long>(identity.byte_test),
                static_cast<unsigned long>(identity.id_revision));
        }

        void remember(const Esc::Identity& identity)
        {
            m_byteTest.store(identity.byte_test);
            m_config.store(identity.hardware_config);
            m_id.store(identity.id_revision);
        }

        void loopback()
        {
            m_select->write(hal::gpio::Level::High);
            std::array<std::uint8_t, 259> tx{}, rx{};
            std::uint32_t random = 0x92535a17U;
            std::uint32_t exchanges{};
            // Odd lengths and offsets exercise the H7 polling FIFO path,
            // including its byte/halfword/word tails and unaligned buffers.
            for (const std::size_t length : { 1U, 2U, 3U, 4U, 7U, 16U, 31U, 64U, 257U }) {
                for (std::size_t pattern = 0; pattern < 32; ++pattern) {
                    cancelled();
                    auto sent = std::span{ tx }.subspan(1, length);
                    auto received = std::span{ rx }.subspan(1, length);
                    for (auto& byte : sent) {
                        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                        byte = pattern == 0 ? 0x00 : pattern == 1 ? 0xff :
                               pattern == 2 ? 0x55 : pattern == 3 ? 0xaa :
                               static_cast<std::uint8_t>(random);
                    }
                    // Complement the expected bytes so an unfilled RX cannot pass.
                    std::transform(sent.begin(), sent.end(), received.begin(), [](auto v) { return ~v; });
                    require(m_bus->exchange(sent, received, 100ms), "loopback transfer");
                    if (!std::equal(sent.begin(), sent.end(), received.begin()))
                        throw std::runtime_error("loopback mismatch; check CN9.28 -> CN9.24 jumper");
                    completed(++exchanges);
                }
            }
            log("Loopback matched all %lu exchanges (1..257 bytes, fixed/random patterns)",
                static_cast<unsigned long>(exchanges));
        }

        void stress(std::uint32_t count)
        {
            const auto baseline = probe();
            const auto start = Clock::now();
            for (std::uint32_t i = 0; i < count; ++i) {
                cancelled();
                const auto value = require(m_esc->identify(100ms), "stress register check");
                remember(value);
                if (value.id_revision != baseline.id_revision)
                    throw std::runtime_error("chip revision changed during stress");
                completed(i + 1);
                if ((i + 1) % 10000 == 0) {
                    log("Progress %lu/%lu sets, zero errors", static_cast<unsigned long>(i + 1),
                        static_cast<unsigned long>(count));
                    std::this_thread::yield();
                }
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            log("%lu sets = %lu single-register reads; zero mismatches; elapsed=%lld ms",
                static_cast<unsigned long>(count), static_cast<unsigned long>(count * 3U),
                static_cast<long long>(ms));
        }

        void run(Operation operation, std::uint32_t count) noexcept
        {
            hardware_spi_test_status = 0x52554e00U;
            hardware_spi_test_completed = 0;
            try {
                if (operation == Operation::Loopback) loopback();
                else if (operation == Operation::Probe) { static_cast<void>(probe()); completed(1); }
                else if (operation == Operation::Boot) boot();
                else stress(count);
                cancelled();
                hardware_spi_test_status = 0x600d600dU;
                log("PASS");
            } catch (const std::exception& error) {
                hardware_spi_test_status = 0xbad00000U;
                log("%s after %lu completed: %s", m_stop.load() ? "STOPPED" : "FAIL",
                    static_cast<unsigned long>(m_completed.load()), error.what());
            }
            m_select->write(hal::gpio::Level::High);
            hardware_spi_test_complete();
            m_busy.store(false);
            log("READY; CS high");
        }

        std::shared_ptr<hal::IDigitalOutput> m_select;
        std::shared_ptr<hal::ISpi> m_bus;
        std::unique_ptr<Esc> m_esc;
        std::atomic_bool m_busy{}, m_stop{};
        std::atomic<std::uint32_t> m_completed{}, m_byteTest{}, m_config{}, m_id{};
        std::jthread m_worker;
    };
}

int main()
{
    try {
        Bench bench;
        Bench::help();
        std::array<char, 80> line{};
        std::size_t size{};
        bool overflow{};
        for (;;) {
            const auto c = std::getchar();
            if (c == EOF) throw std::runtime_error("console input failed");
            if (c == '\0') continue;
            if (c == '\n' || c == '\r') {
                if (overflow) log("REJECTED: line too long");
                else bench.command(std::string_view{ line.data(), size });
                size = 0; overflow = false;
            } else if (!overflow && (c == '\b' || c == 127)) {
                if (size) --size;
            } else if (!overflow) {
                if (size == line.size()) overflow = true;
                else line[size++] = static_cast<char>(c);
            }
        }
    } catch (const std::exception& error) {
        log("FATAL: %s", error.what());
        return 1;
    }
}
