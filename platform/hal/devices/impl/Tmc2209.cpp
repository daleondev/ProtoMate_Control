#include "Tmc2209.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace hal::device
{
    namespace
    {
        auto error(std::errc value) { return std::unexpected(std::make_error_code(value)); }
        // VSENSE=1, VFS=180 mV, Rsense=50 mOhm plus internal 20 mOhm.
        constexpr double fullScale = 180.0 / (0.05 + 0.02) / 1.4142135623730951;
    }

    Tmc2209::Tmc2209(std::shared_ptr<IUart> bus, std::uint8_t address)
      : m_bus{ std::move(bus) }
      , m_address{ address }
    {
        if (!m_bus || address > 3)
            throw std::invalid_argument("invalid TMC2209 bus/address");
    }

    std::uint8_t Tmc2209::crc(std::span<const std::uint8_t> bytes)
    {
        std::uint8_t crc{};
        for (auto byte : bytes)
            for (unsigned bit{}; bit < 8; ++bit) {
                const bool feedback = ((crc >> 7U) ^ (byte & 1U)) != 0;
                crc = static_cast<std::uint8_t>((crc << 1U) ^ (feedback ? 0x07U : 0U));
                byte >>= 1U;
            }
        return crc;
    }

    util::Result<std::uint32_t> Tmc2209::read(std::uint8_t reg)
    {
        std::array<std::uint8_t, 4> request{ 0x05, m_address, reg, 0 };
        request.back() = crc(std::span{ request }.first(3));
        std::array<std::uint8_t, 8> reply{};
        if (auto result = m_bus->exchange(request, reply, std::chrono::milliseconds{ 10 }); !result)
            return std::unexpected(result.error());
        if (reply[0] != 0x05 || reply[1] != 0xFF || reply[2] != reg ||
            crc(std::span{ reply }.first(7)) != reply.back())
            return error(std::errc::protocol_error);
        return (std::uint32_t{ reply[3] } << 24U) | (std::uint32_t{ reply[4] } << 16U) |
               (std::uint32_t{ reply[5] } << 8U) | reply[6];
    }

    util::Result<> Tmc2209::write(std::uint8_t reg, std::uint32_t value)
    {
        const auto before = read(0x02); // IFCNT, not an acknowledgement of arbitrary readback.
        if (!before)
            return std::unexpected(before.error());
        std::array<std::uint8_t, 8> request{ 0x05,
                                             m_address,
                                             static_cast<std::uint8_t>(reg | 0x80U),
                                             static_cast<std::uint8_t>(value >> 24U),
                                             static_cast<std::uint8_t>(value >> 16U),
                                             static_cast<std::uint8_t>(value >> 8U),
                                             static_cast<std::uint8_t>(value),
                                             0 };
        request.back() = crc(std::span{ request }.first(7));
        if (auto result = m_bus->exchange(request, {}, std::chrono::milliseconds{ 10 }); !result)
            return result;
        const auto after = read(0x02);
        if (!after)
            return std::unexpected(after.error());
        if (static_cast<std::uint8_t>(*before + 1U) != static_cast<std::uint8_t>(*after))
            return error(std::errc::protocol_error);
        return {};
    }

    util::Result<std::uint8_t> Tmc2209::currentScale(std::uint16_t milliamps)
    {
        // Never permit freewheel or exceed this board's range. Application
        // applies tighter per-motor limits. Quantization never rounds upward.
        if (milliamps < 100 || milliamps > 1800)
            return error(std::errc::invalid_argument);
        return static_cast<std::uint8_t>(std::floor(milliamps * 32.0 / fullScale) - 1.0);
    }

    std::uint16_t Tmc2209::currentMilliamps(std::uint8_t scale)
    {
        return static_cast<std::uint16_t>((scale + 1U) * fullScale / 32.0);
    }

    util::Result<> Tmc2209::initialize(const Configuration& config)
    {
        m_initialized = false;
        const auto run = currentScale(config.run_milliamps), hold = currentScale(config.hold_milliamps);
        if (!run || !hold || config.hold_milliamps > config.run_milliamps ||
            !std::has_single_bit(config.microsteps) || config.microsteps > 256 ||
            (config.mode != Mode::SpreadCycle && config.mode != Mode::StealthChop) ||
            (config.mode == Mode::StealthChop && *run < 8))
            return error(std::errc::invalid_argument);

        // Bootstrap NODECONF without read access: all nodes power up with an
        // unsuitable reply delay. No other node can reply to a write.
        std::array<std::uint8_t, 8> delay{ 0x05, m_address, 0x83, 0, 0, 2, 0, 0 };
        delay.back() = crc(std::span{ delay }.first(7));
        if (auto r = m_bus->exchange(delay, {}, std::chrono::milliseconds{ 10 }); !r)
            return r;
        auto input = read(0x06);
        if (!input)
            return std::unexpected(input.error());
        if ((*input >> 24U) != 0x21U)
            return error(std::errc::no_such_device);
        if ((*input & 1U) == 0U)
            return error(std::errc::operation_not_permitted); // ENN must be high.
        if (((*input >> 2U) & 3U) != m_address || (*input & (1U << 8U)))
            return error(std::errc::invalid_argument); // Address straps; SPRD jumper must be open.

        m_configuration = config;
        // Digital current scaling: the board potentiometer is intentionally
        // bypassed. External Rsense; preserve STEP/DIR polarity; UART, MRES.
        m_gconf = (1U << 6U) | (1U << 7U) | (1U << 8U) | (config.mode == Mode::SpreadCycle ? 4U : 0U);
        m_chopconf = 0x00020053U | ((8U - std::countr_zero(config.microsteps)) << 24U) |
                     (config.interpolate ? (1U << 28U) : 0U);
        const std::array<std::pair<std::uint8_t, std::uint32_t>, 11> registers{
            { { 0x03, 2U << 8U },
              { 0x00, m_gconf },
              { 0x10, std::uint32_t{ *hold } | (std::uint32_t{ *run } << 8U) | (6U << 16U) },
              { 0x11, 20U },
              { 0x13, 0U },
              { 0x14, 0U }, // no automatic mode/stall switching
              { 0x22, 0U }, // STEP/DIR, never the internal velocity generator
              { 0x42, 0U }, // CoolStep off until mechanically characterized
              { 0x6C, m_chopconf },
              { 0x70, 0xC10D0024U },
              { 0x01, 7U } }
        };
        for (auto [reg, value] : registers)
            if (auto result = write(reg, value); !result)
                return result;
        m_initialized = true;
        if (auto result = verify(); !result) {
            m_initialized = false;
            return result;
        }
        auto state = status();
        if (!state) {
            m_initialized = false;
            return std::unexpected(state.error());
        }
        if (state->fault() || state->reset()) {
            m_initialized = false;
            return error(std::errc::io_error);
        }
        return {};
    }

    util::Result<> Tmc2209::verify()
    {
        if (!m_initialized)
            return error(std::errc::operation_not_permitted);
        for (auto [reg, expected] : std::array<std::pair<std::uint8_t, std::uint32_t>, 3>{
               { { 0x00, m_gconf }, { 0x6C, m_chopconf }, { 0x70, 0xC10D0024U } } }) {
            auto value = read(reg);
            if (!value)
                return std::unexpected(value.error());
            if (*value != expected)
                return error(std::errc::state_not_recoverable);
        }
        return {};
    }

    util::Result<> Tmc2209::prepareBus(IUart& bus, std::span<const std::uint8_t> addresses)
    {
        for (auto address : addresses) {
            if (address > 3)
                return error(std::errc::invalid_argument);
            std::array<std::uint8_t, 8> frame{ 5, address, 0x83, 0, 0, 2, 0, 0 };
            frame.back() = crc(std::span{ frame }.first(7));
            if (auto result = bus.exchange(frame, {}, std::chrono::milliseconds{ 10 }); !result)
                return result;
        }
        return {};
    }

    util::Result<Tmc2209::Status> Tmc2209::status()
    {
        auto input = read(0x06);
        if (!input)
            return std::unexpected(input.error());
        if ((*input >> 24U) != 0x21U)
            return error(std::errc::no_such_device);
        if (((*input >> 2U) & 3U) != m_address || (*input & (1U << 8U)))
            return error(std::errc::state_not_recoverable);
        auto global = read(0x01);
        if (!global)
            return std::unexpected(global.error());
        auto driver = read(0x6F);
        if (!driver)
            return std::unexpected(driver.error());
        auto load = read(0x41);
        if (!load)
            return std::unexpected(load.error());
        return Status{ *global, *driver, *input, static_cast<std::uint16_t>(*load & 0x3FFU) };
    }
}
