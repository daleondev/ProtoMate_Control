#include "Tmc2209.hpp"
#include "hal/devices/util/Tmc2209Protocol.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace hal::device
{
    using namespace util::tmc2209;
    namespace
    {
        auto error(std::errc value) { return std::unexpected(std::make_error_code(value)); }
        // VSENSE=1, VFS=180 mV, Rsense=50 mOhm plus internal 20 mOhm.
        constexpr double FULL_SCALE = 180.0 / (0.05 + 0.02) / std::numbers::sqrt2;
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
            for (unsigned bit{}; bit < BYTE_BITS; ++bit) {
                const bool feedback = ((crc >> (BYTE_BITS - 1U)) ^ (byte & 1U)) != 0;
                crc = static_cast<std::uint8_t>((crc << 1U) ^ (feedback ? CRC_POLYNOMIAL : 0U));
                byte >>= 1U;
            }
        return crc;
    }

    hal::util::Result<std::uint32_t> Tmc2209::read(std::uint8_t reg)
    {
        std::array<std::uint8_t, READ_REQUEST_BYTES> request{ SYNC, m_address, reg, 0 };
        request.back() = crc(std::span{ request }.first(3));
        std::array<std::uint8_t, FRAME_BYTES> reply{};
        if (auto result = m_bus->exchange(request, reply, TRANSACTION_TIMEOUT); !result)
            return std::unexpected(result.error());
        if (reply[0] != SYNC || reply[1] != MASTER_ADDRESS || reply[2] != reg ||
            crc(std::span{ reply }.first(reply.size() - 1U)) != reply.back())
            return error(std::errc::protocol_error);
        return read_word(std::span{ reply }.subspan<DATA_OFFSET, REGISTER_BYTES>());
    }

    // Register API keeps the conventional address, optional width, value order.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    hal::util::Result<> Tmc2209::write(std::uint8_t reg, std::uint32_t value)
    {
        const auto before = read(IFCNT); // IFCNT, not an acknowledgement of arbitrary readback.
        if (!before)
            return std::unexpected(before.error());
        std::array<std::uint8_t, FRAME_BYTES> request{ SYNC,
                                                       m_address,
                                                       static_cast<std::uint8_t>(reg | WRITE_FLAG) };
        write_word(std::span{ request }.subspan<DATA_OFFSET, REGISTER_BYTES>(), value);
        request.back() = crc(std::span{ request }.first(request.size() - 1U));
        if (auto result = m_bus->exchange(request, {}, TRANSACTION_TIMEOUT); !result)
            return result;
        const auto after = read(IFCNT);
        if (!after)
            return std::unexpected(after.error());
        if (static_cast<std::uint8_t>(*before + 1U) != static_cast<std::uint8_t>(*after))
            return error(std::errc::protocol_error);
        return {};
    }
    // NOLINTEND(bugprone-easily-swappable-parameters)

    hal::util::Result<std::uint8_t> Tmc2209::currentScale(std::uint16_t milliamps)
    {
        // Never permit freewheel or exceed this board's range. Application
        // applies tighter per-motor limits. Quantization never rounds upward.
        if (milliamps < MIN_CURRENT_MA || milliamps > MAX_CURRENT_MA)
            return error(std::errc::invalid_argument);
        return static_cast<std::uint8_t>(std::floor(milliamps * CURRENT_SCALE_STEPS / FULL_SCALE) - 1.0);
    }

    std::uint16_t Tmc2209::currentMilliamps(std::uint8_t scale)
    {
        return static_cast<std::uint16_t>((scale + 1U) * FULL_SCALE / CURRENT_SCALE_STEPS);
    }

    hal::util::Result<> Tmc2209::initialize(const Configuration& config)
    {
        m_initialized = false;
        const auto run = currentScale(config.run_milliamps);
        const auto hold = currentScale(config.hold_milliamps);
        if (!run || !hold || config.hold_milliamps > config.run_milliamps ||
            !std::has_single_bit(config.microsteps) || config.microsteps > MAX_MICROSTEPS ||
            (config.mode != Mode::SpreadCycle && config.mode != Mode::StealthChop) ||
            (config.mode == Mode::StealthChop && *run < MIN_STEALTH_CURRENT_SCALE))
            return error(std::errc::invalid_argument);

        // Bootstrap NODECONF without read access: all nodes power up with an
        // unsuitable reply delay. No other node can reply to a write.
        std::array<std::uint8_t, FRAME_BYTES> delay{
            SYNC, m_address, static_cast<std::uint8_t>(NODECONF | WRITE_FLAG), 0, 0, 2, 0, 0
        };
        delay.back() = crc(std::span{ delay }.first(delay.size() - 1U));
        if (auto r = m_bus->exchange(delay, {}, TRANSACTION_TIMEOUT); !r)
            return r;
        auto input = read(IOIN);
        if (!input)
            return std::unexpected(input.error());
        if ((*input >> VERSION_SHIFT) != IC_VERSION)
            return error(std::errc::no_such_device);
        if ((*input & 1U) == 0U)
            return error(std::errc::operation_not_permitted); // ENN must be high.
        if (((*input >> 2U) & 3U) != m_address || (*input & IOIN_SPREAD))
            return error(std::errc::invalid_argument); // Address straps; SPRD jumper must be open.

        m_configuration = config;
        // Digital current scaling: the board potentiometer is intentionally
        // bypassed. External Rsense; preserve STEP/DIR polarity; UART, MRES.
        m_gconf = GCONF_PDN_DISABLE | GCONF_MSTEP_REG_SELECT | GCONF_MULTISTEP_FILTER |
                  (config.mode == Mode::SpreadCycle ? GCONF_SPREADCYCLE : 0U) |
                  (config.index_step ? GCONF_INDEX_STEP : 0U);
        m_chopconf = CHOPCONF_BASE | ((MRES_EXPONENT - std::countr_zero(config.microsteps)) << MRES_SHIFT) |
                     (config.interpolate ? CHOPCONF_INTERPOLATE : 0U);
        const std::array<std::pair<std::uint8_t, std::uint32_t>, 11> registers{
            { { 0x03, 2U << BYTE_BITS },
              { 0x00, m_gconf },
              { 0x10,
                std::uint32_t{ *hold } | (std::uint32_t{ *run } << BYTE_BITS) | (6U << (2U * BYTE_BITS)) },
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

    hal::util::Result<> Tmc2209::verify()
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

    hal::util::Result<> Tmc2209::prepareBus(IUart& bus, std::span<const std::uint8_t> addresses)
    {
        for (auto address : addresses) {
            if (address > 3)
                return error(std::errc::invalid_argument);
            std::array<std::uint8_t, FRAME_BYTES> frame{
                SYNC, address, static_cast<std::uint8_t>(NODECONF | WRITE_FLAG), 0, 0, 2, 0, 0
            };
            frame.back() = crc(std::span{ frame }.first(frame.size() - 1U));
            if (auto result = bus.exchange(frame, {}, TRANSACTION_TIMEOUT); !result)
                return result;
        }
        return {};
    }

    hal::util::Result<Tmc2209::Status> Tmc2209::status()
    {
        auto input = read(IOIN);
        if (!input)
            return std::unexpected(input.error());
        if ((*input >> VERSION_SHIFT) != IC_VERSION)
            return error(std::errc::no_such_device);
        if (((*input >> 2U) & 3U) != m_address || (*input & IOIN_SPREAD))
            return error(std::errc::state_not_recoverable);
        auto global = read(0x01);
        if (!global)
            return std::unexpected(global.error());
        auto driver = read(DRV_STATUS);
        if (!driver)
            return std::unexpected(driver.error());
        auto load = read(SG_RESULT);
        if (!load)
            return std::unexpected(load.error());
        return Status{ *global, *driver, *input, static_cast<std::uint16_t>(*load & LOAD_MASK) };
    }
}
