#pragma once

#include "common/motion/Trajectory.hpp"
#include <memory>

namespace io::motion
{
    // Each bank contains one checkpoint continuation and its complete stop.
    // Two banks bound active + staged storage to the agreed 256 records.
    struct TrajectoryWindow
    {
        static constexpr std::size_t CAPACITY=::motion::trajectory::IO_SEGMENTS/2;
        std::array<::motion::trajectory::Segment,CAPACITY> segments{};
        std::uint16_t continuation{},stopping{};
        std::uint64_t continuation_ticks{},total_ticks{},revision{};
        bool continuation_final{},stopping_final{};
        [[nodiscard]] std::span<const ::motion::trajectory::Segment> nominal() const noexcept
        { return std::span(segments).first(continuation); }
        [[nodiscard]] std::span<const ::motion::trajectory::Segment> stop() const noexcept
        { return std::span(segments).subspan(continuation,stopping); }
    };
    static_assert(sizeof(TrajectoryWindow::segments)*2<=64*1024);

    class ITrajectoryExecutor
    {
      public:
        virtual ~ITrajectoryExecutor()=default;
        // Calls are serialized on the IO application worker. The executor owns
        // hardware locking and must take a coherent position/epoch snapshot.
        virtual ::motion::trajectory::Reply snapshot()=0;
        virtual ::motion::trajectory::Error acquire(std::uint64_t session)=0;
        virtual ::motion::trajectory::Error release(std::uint64_t session)=0;
        // Failure must leave the active program unchanged. Retain ownership of
        // immutable windows until no ISR/DMA producer can access them.
        virtual ::motion::trajectory::Error commit(std::shared_ptr<const TrajectoryWindow> window,
                                                   const ::motion::trajectory::Request& request)=0;
        virtual ::motion::trajectory::Error control(::motion::trajectory::Operation operation)=0;
    };

    class TrajectorySession final
    {
      public:
        explicit TrajectorySession(ITrajectoryExecutor& executor,std::uint64_t boot);
        [[nodiscard]] ::motion::trajectory::Reply request(const ::motion::trajectory::Request& request);
        [[nodiscard]] ::motion::trajectory::Packet request(const ::motion::trajectory::Packet& packet);
        // Call when a stop, reinitialization, transport departure or hardware
        // fault overtakes pending uploads. Nothing is automatically replayed.
        void invalidate() noexcept;
      private:
        ::motion::trajectory::Reply reply(::motion::trajectory::Error error=::motion::trajectory::Error::None);
        ::motion::trajectory::Error upload(const ::motion::trajectory::Request& request);
        ::motion::trajectory::Error commit(const ::motion::trajectory::Request& request);
        ITrajectoryExecutor* m_executor;
        const std::uint64_t m_boot;
        std::uint64_t m_session{},m_nextSession{1},m_generation{},m_configuration{},m_revision{};
        std::array<std::shared_ptr<TrajectoryWindow>,2> m_banks;
        unsigned m_staging{};
        struct Chunk { std::uint32_t index{},crc{};std::uint8_t count{};::motion::trajectory::Branch branch{};bool final{}; };
        std::array<Chunk,TrajectoryWindow::CAPACITY> m_chunks{};
        std::size_t m_chunkCount{};
        ::motion::trajectory::Counts m_anchor{};
    };
}
