#include "Ethernet.hpp"

#include <arpa/inet.h>
#include <linux/ethtool.h>
#include <linux/filter.h>
#include <linux/if_packet.h>
#include <linux/sockios.h>
#include <net/ethernet.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <system_error>
#include <utility>

// This file is the POSIX ABI boundary. These casts and C-array conversions are
// required by socket, ioctl, and BPF interfaces.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay,
//             cppcoreguidelines-pro-type-reinterpret-cast,
//             cppcoreguidelines-pro-type-vararg)
namespace hal
{
    namespace
    {
        constexpr std::uint64_t NANOSECONDS_PER_SECOND{ 1'000'000'000ULL };
        constexpr std::uint64_t NANOSECONDS_PER_MILLISECOND{ 1'000'000ULL };

        [[nodiscard]] constexpr auto ether_type_value(IEthernet::EtherType type) noexcept -> std::uint16_t
        {
            return std::to_underlying(type);
        }

        class PthreadLock
        {
          public:
            explicit PthreadLock(pthread_mutex_t& mutex) noexcept
              : m_mutex{ mutex }
            {
                if (pthread_mutex_lock(&m_mutex) != 0) {
                    std::terminate();
                }
            }

            ~PthreadLock()
            {
                if (pthread_mutex_unlock(&m_mutex) != 0) {
                    std::terminate();
                }
            }

            PthreadLock(const PthreadLock&) = delete;
            PthreadLock& operator=(const PthreadLock&) = delete;
            PthreadLock(PthreadLock&&) = delete;
            PthreadLock& operator=(PthreadLock&&) = delete;

          private:
            pthread_mutex_t& m_mutex;
        };

        [[nodiscard]] auto errno_result() noexcept -> std::unexpected<std::error_code>
        {
            return std::unexpected(std::error_code{ errno, std::generic_category() });
        }

        template<typename T = void>
        [[nodiscard]] auto error_result(std::errc error) noexcept -> util::Result<T>
        {
            return std::unexpected(std::make_error_code(error));
        }

        [[nodiscard]] auto monotonic_nanoseconds() noexcept -> std::uint64_t
        {
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                return 0U;
            }
            return (static_cast<std::uint64_t>(now.tv_sec) * NANOSECONDS_PER_SECOND) +
                   static_cast<std::uint64_t>(now.tv_nsec);
        }

        [[nodiscard]] auto make_deadline(std::chrono::milliseconds timeout) noexcept -> std::uint64_t
        {
            const std::uint64_t now{ monotonic_nanoseconds() };
            if (timeout.count() <= 0) {
                return now;
            }
            const auto milliseconds{ static_cast<std::uint64_t>(timeout.count()) };
            if (milliseconds >
                (std::numeric_limits<std::uint64_t>::max() - now) / NANOSECONDS_PER_MILLISECOND) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            return now + (milliseconds * NANOSECONDS_PER_MILLISECOND);
        }

        [[nodiscard]] auto poll_timeout(std::uint64_t deadline) noexcept -> int
        {
            const std::uint64_t now{ monotonic_nanoseconds() };
            if (deadline <= now) {
                return 0;
            }
            const std::uint64_t remaining{ deadline - now };
            const std::uint64_t milliseconds{ (remaining + NANOSECONDS_PER_MILLISECOND - 1U) /
                                              NANOSECONDS_PER_MILLISECOND };
            return static_cast<int>(std::min<std::uint64_t>(milliseconds, INT_MAX));
        }

        auto copy_interface_name(ifreq& request, const std::array<char, IFNAMSIZ>& name) noexcept -> void
        {
            std::memcpy(request.ifr_name, name.data(), name.size());
            request.ifr_name[IFNAMSIZ - 1U] = '\0';
        }

        struct FilterConfiguration
        {
            int socket_handle;
            IEthernet::EtherType ether_type;
        };

        [[nodiscard]] auto attach_ether_type_filter(FilterConfiguration configuration) noexcept
          -> util::Result<>
        {
            std::array<sock_filter, 12U> instructions{{
                BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12U),
                BPF_JUMP(
                  BPF_JMP | BPF_JEQ | BPF_K, ether_type_value(configuration.ether_type), 8U, 0U),
                BPF_JUMP(
                  BPF_JMP | BPF_JEQ | BPF_K, ether_type_value(IEthernet::EtherType::VLAN), 1U, 0U),
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                         ether_type_value(IEthernet::EtherType::SERVICE_VLAN),
                         0U,
                         7U),
                BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 16U),
                BPF_JUMP(
                  BPF_JMP | BPF_JEQ | BPF_K, ether_type_value(configuration.ether_type), 4U, 0U),
                BPF_JUMP(
                  BPF_JMP | BPF_JEQ | BPF_K, ether_type_value(IEthernet::EtherType::VLAN), 1U, 0U),
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                         ether_type_value(IEthernet::EtherType::SERVICE_VLAN),
                         0U,
                         3U),
                BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 20U),
                BPF_JUMP(
                  BPF_JMP | BPF_JEQ | BPF_K, ether_type_value(configuration.ether_type), 0U, 1U),
                BPF_STMT(BPF_RET | BPF_K, 0xFFFFU),
                BPF_STMT(BPF_RET | BPF_K, 0U),
            }};
            sock_fprog program{ .len = static_cast<unsigned short>(instructions.size()),
                                .filter = instructions.data() };
            if (setsockopt(configuration.socket_handle,
                           SOL_SOCKET,
                           SO_ATTACH_FILTER,
                           &program,
                           sizeof(program)) != 0) {
                return errno_result();
            }
            return {};
        }

        [[nodiscard]] auto poll_descriptor(pollfd& descriptor, std::uint64_t deadline) noexcept -> int
        {
            while (true) {
                const int status{ ::poll(&descriptor, 1U, poll_timeout(deadline)) };
                if (status >= 0 || errno != EINTR) {
                    return status;
                }
            }
        }
    }

    Ethernet::Ethernet(Configuration configuration) noexcept
      : m_receiveEtherType{ configuration.receive_ether_type }
      , m_promiscuous{ configuration.promiscuous }
    {
        if (configuration.interface_name.size() < m_interfaceName.size()) {
            std::memcpy(m_interfaceName.data(),
                        configuration.interface_name.data(),
                        configuration.interface_name.size());
        }
        if (pthread_mutex_init(&m_lifecycleMutex, nullptr) != 0 ||
            pthread_mutex_init(&m_receiveMutex, nullptr) != 0 ||
            pthread_mutex_init(&m_transmitMutex, nullptr) != 0) {
            std::terminate();
        }
    }

    Ethernet::~Ethernet()
    {
        static_cast<void>(stop());
        if (pthread_mutex_destroy(&m_transmitMutex) != 0 || pthread_mutex_destroy(&m_receiveMutex) != 0 ||
            pthread_mutex_destroy(&m_lifecycleMutex) != 0) {
            std::terminate();
        }
    }

    auto Ethernet::start() noexcept -> util::Result<>
    {
        PthreadLock lock{ m_lifecycleMutex };
        if (m_socket >= 0) {
            return {};
        }
        if (m_interfaceName.front() == '\0') {
            return error_result(std::errc::invalid_argument);
        }

        m_interfaceIndex = if_nametoindex(m_interfaceName.data());
        if (m_interfaceIndex == 0U) {
            return errno_result();
        }

        const int socket_handle{ ::socket(
          AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL)) };
        if (socket_handle < 0) {
            return errno_result();
        }

        sockaddr_ll address{};
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(ETH_P_ALL);
        address.sll_ifindex = static_cast<int>(m_interfaceIndex);
        if (::bind(socket_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            const auto error{ errno_result() };
            static_cast<void>(::close(socket_handle));
            return error;
        }

#if defined(PACKET_IGNORE_OUTGOING)
        constexpr int ignore_outgoing{ 1 };
        static_cast<void>(setsockopt(
          socket_handle, SOL_PACKET, PACKET_IGNORE_OUTGOING, &ignore_outgoing, sizeof(ignore_outgoing)));
#endif

#if defined(PACKET_QDISC_BYPASS)
        constexpr int bypass_queueing_discipline{ 1 };
        static_cast<void>(setsockopt(socket_handle,
                                     SOL_PACKET,
                                     PACKET_QDISC_BYPASS,
                                     &bypass_queueing_discipline,
                                     sizeof(bypass_queueing_discipline)));
#endif

        if (m_receiveEtherType) {
            const auto filter_result{ attach_ether_type_filter(
              FilterConfiguration{ .socket_handle = socket_handle,
                                   .ether_type = *m_receiveEtherType }) };
            if (!filter_result) {
                static_cast<void>(::close(socket_handle));
                return filter_result;
            }
        }

        if (m_promiscuous) {
            packet_mreq membership{};
            membership.mr_ifindex = static_cast<int>(m_interfaceIndex);
            membership.mr_type = PACKET_MR_PROMISC;
            if (setsockopt(
                  socket_handle, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) {
                const auto error{ errno_result() };
                static_cast<void>(::close(socket_handle));
                return error;
            }
        }

        ifreq request{};
        copy_interface_name(request, m_interfaceName);
        if (ioctl(socket_handle, SIOCGIFHWADDR, &request) != 0) {
            const auto error{ errno_result() };
            static_cast<void>(::close(socket_handle));
            return error;
        }
        std::memcpy(m_macAddress.data(), request.ifr_hwaddr.sa_data, m_macAddress.size());
        m_socket = socket_handle;
        return {};
    }

    auto Ethernet::stop() noexcept -> util::Result<>
    {
        // Prevent close/reuse of the descriptor while another thread is in a
        // send or receive syscall. Transmit and receive remain full duplex.
        PthreadLock transmit_lock{ m_transmitMutex };
        PthreadLock receive_lock{ m_receiveMutex };
        PthreadLock lock{ m_lifecycleMutex };
        if (m_socket < 0) {
            return {};
        }
        const int socket_handle{ m_socket };
        m_socket = -1;
        m_interfaceIndex = 0U;
        if (::close(socket_handle) != 0) {
            return errno_result();
        }
        return {};
    }

    auto Ethernet::isRunning() const noexcept -> bool
    {
        PthreadLock lock{ m_lifecycleMutex };
        return m_socket >= 0;
    }

    auto Ethernet::getMacAddress() const noexcept -> MacAddress
    {
        PthreadLock lock{ m_lifecycleMutex };
        return m_macAddress;
    }

    auto Ethernet::getLinkInfo() const noexcept -> util::Result<LinkInfo>
    {
        if (m_interfaceName.front() == '\0') {
            return error_result<LinkInfo>(std::errc::invalid_argument);
        }

        const int control_socket{ ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0) };
        if (control_socket < 0) {
            return errno_result();
        }

        ifreq request{};
        copy_interface_name(request, m_interfaceName);
        if (ioctl(control_socket, SIOCGIFFLAGS, &request) != 0) {
            const auto error{ errno_result() };
            static_cast<void>(::close(control_socket));
            return error;
        }

        LinkInfo info{ .up = (request.ifr_flags & IFF_UP) != 0 && (request.ifr_flags & IFF_RUNNING) != 0 };
        ethtool_cmd command{};
        command.cmd = ETHTOOL_GSET;
        copy_interface_name(request, m_interfaceName);
        request.ifr_data = reinterpret_cast<char*>(&command);
        if (ioctl(control_socket, SIOCETHTOOL, &request) == 0) {
            const std::uint32_t speed{ ethtool_cmd_speed(&command) };
            if (speed != std::numeric_limits<std::uint32_t>::max()) {
                info.speed_mbps = speed;
            }
            if (command.duplex == DUPLEX_FULL) {
                info.duplex = Duplex::Full;
            }
            else if (command.duplex == DUPLEX_HALF) {
                info.duplex = Duplex::Half;
            }
        }
        static_cast<void>(::close(control_socket));
        return info;
    }

    auto Ethernet::transmit(std::span<const std::byte> frame, std::chrono::milliseconds timeout) noexcept
      -> util::Result<>
    {
        if (frame.size() < ETHERNET_HEADER_SIZE || frame.size() > MAX_FRAME_SIZE || timeout.count() < 0) {
            return error_result(std::errc::invalid_argument);
        }

        PthreadLock transmit_lock{ m_transmitMutex };
        int socket_handle{};
        unsigned int interface_index{};
        MacAddress source{};
        {
            PthreadLock lifecycle_lock{ m_lifecycleMutex };
            if (m_socket < 0) {
                return error_result(std::errc::not_connected);
            }
            socket_handle = m_socket;
            interface_index = m_interfaceIndex;
            source = m_macAddress;
        }

        std::array<std::byte, MAX_FRAME_SIZE> packet{};
        std::ranges::copy(frame, packet.begin());
        for (std::size_t index{}; index < source.size(); ++index) {
            packet[MAC_ADDRESS_SIZE + index] = static_cast<std::byte>(source[index]);
        }

        pollfd descriptor{ .fd = socket_handle, .events = POLLOUT, .revents = 0 };
        const std::uint64_t deadline{ make_deadline(timeout) };
        const int poll_status{ poll_descriptor(descriptor, deadline) };
        if (poll_status < 0) {
            return errno_result();
        }
        if (poll_status == 0) {
            return error_result(std::errc::timed_out);
        }

        sockaddr_ll destination{};
        destination.sll_family = AF_PACKET;
        const auto outer_type{ frameOuterEtherType(frame) };
        destination.sll_protocol = htons(outer_type ? ether_type_value(*outer_type) : ETH_P_ALL);
        destination.sll_ifindex = static_cast<int>(interface_index);
        destination.sll_halen = ETH_ALEN;
        std::memcpy(destination.sll_addr, packet.data(), ETH_ALEN);

        const std::size_t wire_size{ std::max(frame.size(), MIN_FRAME_SIZE) };
        const ssize_t sent{ sendto(socket_handle,
                                   packet.data(),
                                   wire_size,
                                   0,
                                   reinterpret_cast<const sockaddr*>(&destination),
                                   sizeof(destination)) };
        if (sent < 0) {
            return errno_result();
        }
        if (sent != static_cast<ssize_t>(wire_size)) {
            return error_result(std::errc::io_error);
        }
        return {};
    }

    // The loop deliberately combines readiness, packet-origin, filter, and
    // caller-buffer validation so the timeout applies to the complete receive.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    auto Ethernet::receive(std::span<std::byte> frame, std::chrono::milliseconds timeout) noexcept
      -> util::Result<std::size_t>
    {
        if (frame.empty() || timeout.count() < 0) {
            return error_result<std::size_t>(std::errc::invalid_argument);
        }

        PthreadLock receive_lock{ m_receiveMutex };
        int socket_handle{};
        {
            PthreadLock lifecycle_lock{ m_lifecycleMutex };
            if (m_socket < 0) {
                return error_result<std::size_t>(std::errc::not_connected);
            }
            socket_handle = m_socket;
        }

        const std::uint64_t deadline{ make_deadline(timeout) };
        std::array<std::byte, MAX_FRAME_SIZE> packet{};
        while (true) {
            pollfd descriptor{ .fd = socket_handle, .events = POLLIN, .revents = 0 };
            const int poll_status{ poll_descriptor(descriptor, deadline) };
            if (poll_status < 0) {
                return errno_result();
            }
            if (poll_status == 0) {
                return error_result<std::size_t>(std::errc::timed_out);
            }

            sockaddr_ll source{};
            socklen_t source_size{ sizeof(source) };
            const ssize_t received{ recvfrom(socket_handle,
                                             packet.data(),
                                             packet.size(),
                                             MSG_TRUNC,
                                             reinterpret_cast<sockaddr*>(&source),
                                             &source_size) };
            if (received < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                return errno_result();
            }
            if (source.sll_pkttype == PACKET_OUTGOING) {
                continue;
            }

            const auto received_size{ static_cast<std::size_t>(received) };
            if (received_size > packet.size()) {
                return error_result<std::size_t>(std::errc::message_size);
            }
            const std::span<const std::byte> received_frame{ packet.data(), received_size };
            if (m_receiveEtherType && frameEtherType(received_frame) != m_receiveEtherType) {
                continue;
            }
            if (frame.size() < received_size) {
                return error_result<std::size_t>(std::errc::no_buffer_space);
            }
            std::ranges::copy(received_frame, frame.begin());
            return received_size;
        }
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay,
//           cppcoreguidelines-pro-type-reinterpret-cast,
//           cppcoreguidelines-pro-type-vararg)
