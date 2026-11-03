#include "Trajectory.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace motion::trajectory
{
    namespace
    {
        struct Writer
        {
            Packet packet;
            template<class T>
            void integer(T value)
            {
                using U = std::make_unsigned_t<T>;
                const U bits = std::bit_cast<U>(value);
                if (packet.size + sizeof(T) > Packet::CAPACITY) {
                    throw std::length_error("trajectory frame");
                }
                for (std::size_t i = 0; i < sizeof(T); ++i) {
                    packet.bytes[packet.size++] = std::byte((bits >> (8 * i)) & 255U);
                }
            }
            void number(double value) { integer(std::bit_cast<std::uint64_t>(value)); }
            Packet finish()
            {
                integer(checksum(std::span(packet.bytes).first(packet.size)));
                return packet;
            }
        };
        struct Reader
        {
            const Packet& packet;
            std::size_t position{};
            bool valid{ true };
            template<class T>
            T integer()
            {
                using U = std::make_unsigned_t<T>;
                if (position + sizeof(T) > packet.size) {
                    valid = false;
                    return {};
                }
                U value{};
                for (std::size_t i = 0; i < sizeof(T); ++i) {
                    value |= static_cast<U>(std::to_integer<unsigned>(packet.bytes[position++])) << (8 * i);
                }
                return std::bit_cast<T>(value);
            }
            double number() { return std::bit_cast<double>(integer<std::uint64_t>()); }
            bool boolean()
            {
                const auto value = integer<std::uint8_t>();
                valid &= value <= 1;
                return value != 0;
            }
            bool finish()
            {
                const auto size = position;
                const auto crc = integer<std::uint32_t>();
                return valid && position == packet.size &&
                       crc == checksum(std::span(packet.bytes).first(size));
            }
        };
        void write(Writer& w, const KinematicState& s)
        {
            w.number(s.position);
            w.number(s.velocity);
            w.number(s.acceleration);
        }
        KinematicState readState(Reader& r) { return { r.number(), r.number(), r.number() }; }
        void write(Writer& w, const Limits& l)
        {
            for (double v : { l.minimum, l.maximum, l.velocity, l.acceleration, l.deceleration, l.jerk }) {
                w.number(v);
            }
        }
        Limits readLimits(Reader& r)
        {
            return { r.number(), r.number(), r.number(), r.number(), r.number(), r.number() };
        }
        void write(Writer& w, const Segment& s)
        {
            w.integer(s.ticks);
            for (const auto& p : s.motors) {
                for (double c : p.coefficients) {
                    w.number(c);
                }
            }
            w.integer(s.job);
            w.number(s.path_begin);
            w.number(s.path_end);
        }
        Segment readSegment(Reader& r)
        {
            Segment s;
            s.ticks = r.integer<std::uint64_t>();
            for (auto& p : s.motors) {
                for (double& c : p.coefficients) {
                    c = r.number();
                }
            }
            s.job = r.integer<std::uint64_t>();
            s.path_begin = r.number();
            s.path_end = r.number();
            return s;
        }
        void write(Writer& w, const Capabilities& c)
        {
            w.integer(c.version);
            w.integer(c.tick_ns);
            for (double n : c.step_radians) {
                w.number(n);
            }
            for (double n : c.reference_steps) {
                w.number(n);
            }
            for (const auto& l : c.limits) {
                write(w, l);
            }
            w.integer(c.hardware_fingerprint);
            w.integer(c.minimum_period_ticks);
            w.integer(c.high_ticks);
            w.integer(c.direction_setup_ticks);
            w.integer(c.direction_hold_ticks);
            w.integer(c.capacity);
            w.integer(std::uint8_t(c.coordinated_execution));
        }
        Capabilities readCapabilities(Reader& r)
        {
            Capabilities c;
            c.version = r.integer<std::uint16_t>();
            c.tick_ns = r.integer<std::uint32_t>();
            for (double& n : c.step_radians) {
                n = r.number();
            }
            for (double& n : c.reference_steps) {
                n = r.number();
            }
            for (auto& l : c.limits) {
                l = readLimits(r);
            }
            c.hardware_fingerprint = r.integer<std::uint64_t>();
            c.minimum_period_ticks = r.integer<std::uint32_t>();
            c.high_ticks = r.integer<std::uint32_t>();
            c.direction_setup_ticks = r.integer<std::uint32_t>();
            c.direction_hold_ticks = r.integer<std::uint32_t>();
            c.capacity = r.integer<std::uint16_t>();
            c.coordinated_execution = r.boolean();
            return c;
        }
        bool near(double a, double b, double absolute) noexcept
        {
            return std::isfinite(a) && std::isfinite(b) &&
                   std::abs(a - b) <= absolute + 2e-12 * std::max(std::abs(a), std::abs(b));
        }
    }

    std::string_view name(Error e) noexcept
    {
        constexpr std::array names{ "none",
                                    "invalid input",
                                    "configuration mismatch/unverified",
                                    "unreferenced",
                                    "busy",
                                    "stale generation/cursor",
                                    "capacity",
                                    "discontinuous trajectory",
                                    "position limit",
                                    "velocity limit",
                                    "acceleration limit",
                                    "jerk limit",
                                    "timing",
                                    "protocol",
                                    "checksum",
                                    "out of order",
                                    "unsupported",
                                    "fault",
                                    "outcome unknown",
                                    "unreachable",
                                    "singularity",
                                    "workspace",
                                    "path tolerance",
                                    "subdivision limit",
                                    "lease expired",
                                    "starved",
                                    "cancelled",
                                    "too late",
                                    "no stopping path" };
        const auto n = static_cast<unsigned>(e);
        return n < names.size() ? names[n] : "invalid error";
    }

    MotorStates Segment::at(double u) const noexcept
    {
        MotorStates result;
        const double duration = ticks * TICK_SECONDS;
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = { motors[i].at(u),
                          motors[i].at(u, 1) / duration,
                          motors[i].at(u, 2) / (duration * duration) };
        }
        return result;
    }

    Error validate(const Segment& s, const std::array<Limits, 3>& limits) noexcept
    {
        if (!s.ticks || s.ticks > 600'000'000 || !std::isfinite(s.path_begin) || !std::isfinite(s.path_end) ||
            s.path_begin < 0 || s.path_end < s.path_begin || s.path_end > 1) {
            return Error::InvalidInput;
        }
        const double t = s.ticks * TICK_SECONDS;
        for (std::size_t i = 0; i < 3; ++i) {
            const auto& p = s.motors[i];
            const auto& l = limits[i];
            if (!p.finite()) {
                return Error::InvalidInput;
            }
            for (double n : { l.minimum, l.maximum, l.velocity, l.acceleration, l.deceleration, l.jerk }) {
                if (!std::isfinite(n)) {
                    return Error::Configuration;
                }
            }
            if (l.minimum < -MAX_POSITION || l.maximum > MAX_POSITION || l.minimum > l.maximum ||
                l.velocity <= 0 || l.acceleration <= 0 || l.deceleration <= 0 || l.jerk <= 0) {
                return Error::Configuration;
            }
            const auto position = p.range();
            if (!position.finite() || position.minimum < l.minimum - 1e-4 ||
                position.maximum > l.maximum + 1e-4) {
                return Error::PositionLimit;
            }
            // Bernstein bounds are conservative even between sample points.
            if (p.range(0, 1, 1).magnitude() > l.velocity * t * (1 + 1e-9) + 1e-10) {
                return Error::VelocityLimit;
            }
            if (p.range(0, 1, 2).magnitude() >
                std::min(l.acceleration, l.deceleration) * t * t * (1 + 1e-9) + 1e-10) {
                return Error::AccelerationLimit;
            }
            if (p.range(0, 1, 3).magnitude() > l.jerk * t * t * t * (1 + 1e-9) + 1e-10) {
                return Error::JerkLimit;
            }
        }
        return Error::None;
    }

    bool continuous(const MotorStates& a, const MotorStates& b) noexcept
    {
        for (std::size_t i = 0; i < 3; ++i) {
            if (!near(a[i].position, b[i].position, 1e-5) || !near(a[i].velocity, b[i].velocity, 1e-4) ||
                !near(a[i].acceleration, b[i].acceleration, 1e-3)) {
                return false;
            }
        }
        return true;
    }
    bool atRest(const MotorStates& s) noexcept
    {
        return std::ranges::all_of(s, [](const auto& a) {
            return std::isfinite(a.position) && near(a.velocity, 0, 1e-5) && near(a.acceleration, 0, 1e-4);
        });
    }
    std::expected<std::int64_t, Error> quantize(double position) noexcept
    {
        if (!std::isfinite(position) || std::abs(position) > MAX_POSITION) {
            return std::unexpected(Error::PositionLimit);
        }
        return static_cast<std::int64_t>(std::floor(position + 0.5));
    }

    std::uint32_t checksum(std::span<const std::byte> data) noexcept
    {
        std::uint32_t crc = 0xffffffffU;
        for (const auto byte : data) {
            crc ^= std::to_integer<std::uint8_t>(byte);
            for (unsigned i = 0; i < 8; ++i) {
                crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
            }
        }
        return ~crc;
    }

    std::uint64_t fingerprint(const Capabilities& c) noexcept
    {
        // FNV-1a on canonical numeric fields; excludes runtime availability.
        auto canonical = c;
        canonical.hardware_fingerprint = 0;
        canonical.coordinated_execution = false;
        Writer w;
        write(w, canonical);
        std::uint64_t hash = 14695981039346656037ULL;
        for (auto b : std::span(w.packet.bytes).first(w.packet.size)) {
            hash ^= std::to_integer<std::uint8_t>(b);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    Packet encode(const Request& r)
    {
        if (r.count > UPLOAD_SEGMENTS) {
            throw std::length_error("trajectory upload count");
        }
        Writer w;
        w.integer(std::uint8_t{ 1 });
        w.integer(std::uint8_t(r.operation));
        w.integer(r.version);
        for (auto n : { r.boot, r.session, r.generation, r.configuration, r.revision, r.splice_tick }) {
            w.integer(n);
        }
        for (auto n : r.expected_position) {
            w.integer(n);
        }
        w.integer(r.index);
        w.integer(r.count);
        w.integer(std::uint8_t(r.branch));
        w.integer(std::uint8_t(r.final));
        for (std::size_t i = 0; i < r.count; ++i) {
            write(w, r.segments[i]);
        }
        return w.finish();
    }
    Packet encode(const Reply& r)
    {
        Writer w;
        w.integer(std::uint8_t{ 2 });
        w.integer(std::uint8_t(r.error));
        w.integer(std::uint8_t(r.state));
        write(w, r.capabilities);
        for (auto n : { r.boot, r.session, r.generation, r.revision, r.tick, r.splice_tick, r.job }) {
            w.integer(n);
        }
        w.integer(r.next_index);
        w.integer(r.stopping_index);
        w.integer(r.credits);
        w.integer(r.referenced);
        w.integer(std::uint8_t(r.enabled));
        w.integer(std::uint8_t(r.counts_exact));
        for (auto n : r.position) {
            w.integer(n);
        }
        for (const auto& s : r.boundary) {
            write(w, s);
        }
        w.number(r.path);
        return w.finish();
    }
    std::expected<Request, Error> decodeRequest(const Packet& p) noexcept
    {
        if (p.size > p.bytes.size() || p.size < 4) {
            return std::unexpected(Error::Protocol);
        }
        Reader r{ p };
        if (r.integer<std::uint8_t>() != 1) {
            return std::unexpected(Error::Protocol);
        }
        Request v;
        v.operation = Operation(r.integer<std::uint8_t>());
        v.version = r.integer<std::uint16_t>();
        v.boot = r.integer<std::uint64_t>();
        v.session = r.integer<std::uint64_t>();
        v.generation = r.integer<std::uint64_t>();
        v.configuration = r.integer<std::uint64_t>();
        v.revision = r.integer<std::uint64_t>();
        v.splice_tick = r.integer<std::uint64_t>();
        for (auto& n : v.expected_position) {
            n = r.integer<std::int64_t>();
        }
        v.index = r.integer<std::uint32_t>();
        v.count = r.integer<std::uint8_t>();
        v.branch = Branch(r.integer<std::uint8_t>());
        v.final = r.boolean();
        if (v.count > UPLOAD_SEGMENTS || v.operation > Operation::Home || v.branch > Branch::Stopping ||
            v.version != VERSION) {
            return std::unexpected(Error::Protocol);
        }
        for (std::size_t i = 0; i < v.count; ++i) {
            v.segments[i] = readSegment(r);
        }
        if (!r.finish()) {
            return std::unexpected(Error::Checksum);
        }
        return v;
    }
    std::expected<Reply, Error> decodeReply(const Packet& p) noexcept
    {
        if (p.size > p.bytes.size() || p.size < 4) {
            return std::unexpected(Error::Protocol);
        }
        Reader r{ p };
        if (r.integer<std::uint8_t>() != 2) {
            return std::unexpected(Error::Protocol);
        }
        Reply v;
        v.error = Error(r.integer<std::uint8_t>());
        v.state = State(r.integer<std::uint8_t>());
        v.capabilities = readCapabilities(r);
        v.boot = r.integer<std::uint64_t>();
        v.session = r.integer<std::uint64_t>();
        v.generation = r.integer<std::uint64_t>();
        v.revision = r.integer<std::uint64_t>();
        v.tick = r.integer<std::uint64_t>();
        v.splice_tick = r.integer<std::uint64_t>();
        v.job = r.integer<std::uint64_t>();
        v.next_index = r.integer<std::uint32_t>();
        v.stopping_index = r.integer<std::uint32_t>();
        v.credits = r.integer<std::uint16_t>();
        v.referenced = r.integer<std::uint8_t>();
        v.enabled = r.boolean();
        v.counts_exact = r.boolean();
        for (auto& n : v.position) {
            n = r.integer<std::int64_t>();
        }
        for (auto& s : v.boundary) {
            s = readState(r);
        }
        v.path = r.number();
        if (v.error > Error::NoStoppingPath || v.state > State::Unknown || v.referenced > 7 ||
            v.capabilities.version != VERSION) {
            return std::unexpected(Error::Protocol);
        }
        if (!r.finish()) {
            return std::unexpected(Error::Checksum);
        }
        return v;
    }
}

namespace pnm::utils::memory
{
    using Packet = motion::trajectory::Packet;
    std::size_t SerializationAdapter<Packet>::bufferSize(const Packet& p)
    {
        if (p.size > p.bytes.size()) {
            throw std::length_error("trajectory packet size");
        }
        return p.size;
    }
    void SerializationAdapter<Packet>::serialize(const Packet& p, std::span<std::byte> out)
    {
        if (out.size() != bufferSize(p)) {
            throw std::length_error("trajectory packet output");
        }
        std::copy_n(p.bytes.begin(), p.size, out.begin());
    }
    void SerializationAdapter<Packet>::deserialize(std::span<const std::byte> in, Packet& p)
    {
        if (in.size() > p.bytes.size()) {
            throw std::length_error("trajectory packet input");
        }
        p.size = static_cast<std::uint16_t>(in.size());
        std::ranges::copy(in, p.bytes.begin());
    }
}
