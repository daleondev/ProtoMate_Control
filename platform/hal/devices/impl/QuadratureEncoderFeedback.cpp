#include "QuadratureEncoderFeedback.hpp"

#ifdef HAL_PLATFORM_STM32
#include "hal/stm32/InterruptGuard.hpp"
#else
#include <mutex>
#endif

#include <optional>
#include <stdexcept>
#include <utility>

namespace hal::device
{
    using namespace pnm::units::literals;

    struct QuadratureEncoderFeedback::State
    {
#ifdef HAL_PLATFORM_STM32
        struct Mutex {};
        struct Guard
        {
            explicit Guard(Mutex&) noexcept {}
            stm32::InterruptGuard interrupt_guard;
        };
#else
        using Mutex = std::mutex;
        using Guard = std::lock_guard<Mutex>;
#endif
        Mutex mutex;
        const pnm::units::Angle count_angle;
        pnm::units::Angle offset{};
        std::optional<IQuadratureEncoder::Sample> previous;
        Sample latest{ std::unexpected(std::make_error_code(std::errc::no_message_available)) };
        Callback callback;

        explicit State(std::uint32_t counts) : count_angle{ 360_deg / static_cast<double>(counts) } {}

        void sample(const IQuadratureEncoder::Sample& value) noexcept
        {
            const Guard guard{ mutex };
            latest = { std::unexpected(std::make_error_code(std::errc::state_not_recoverable)),
                       0_rpm, value.timestamp, true };
            if (value.position) {
                auto velocity{ 0_rpm };
                if (value.running && previous && previous->running && value.timestamp > previous->timestamp) {
                    const auto current{ *value.position }, prior{ *previous->position };
                    // Unsigned subtraction avoids signed overflow across zero.
                    const auto magnitude{ current >= prior
                        ? static_cast<std::uint64_t>(current) - static_cast<std::uint64_t>(prior)
                        : static_cast<std::uint64_t>(prior) - static_cast<std::uint64_t>(current) };
                    const auto delta{ (current >= prior ? 1.0 : -1.0) * static_cast<double>(magnitude) };
                    velocity = count_angle * delta / pnm::units::Time{ value.timestamp - previous->timestamp };
                }
                const auto position{ offset + count_angle * static_cast<double>(*value.position) };
                if (position.isFinite() && velocity.isFinite())
                    latest = { position, velocity, value.timestamp, false };
                previous = value;
            }
            else {
                previous.reset();
            }
            if (callback) callback(latest);
        }
    };

    QuadratureEncoderFeedback::QuadratureEncoderFeedback(std::shared_ptr<IQuadratureEncoder> encoder,
                                                         std::uint32_t counts,
                                                         std::shared_ptr<IDigitalInput> index)
      : m_encoder{ std::move(encoder) }, m_index{ std::move(index) }
    {
        if (!m_encoder || counts == 0U)
            throw std::invalid_argument("quadrature feedback needs an encoder and nonzero resolution");
        m_state = std::make_shared<State>(counts);
        subscribe();
    }

    void QuadratureEncoderFeedback::subscribe()
    {
        m_encoder->setSampleCallback([state = m_state](const IQuadratureEncoder::Sample& sample) noexcept {
            state->sample(sample);
        });
    }

    QuadratureEncoderFeedback::~QuadratureEncoderFeedback()
    {
        m_encoder->clearSampleCallback();
        static_cast<void>(m_encoder->stop());
        clearCallback();
    }

    pnm::units::Angle QuadratureEncoderFeedback::resolution() const noexcept { return m_state->count_angle; }
    hal::util::Result<> QuadratureEncoderFeedback::start() noexcept { return m_encoder->start(); }
    hal::util::Result<> QuadratureEncoderFeedback::stop() noexcept { return m_encoder->stop(); }

    void QuadratureEncoderFeedback::setCallback(Callback callback)
    {
        const State::Guard guard{ m_state->mutex };
        m_state->callback = std::move(callback);
        if (m_state->callback) m_state->callback(m_state->latest);
    }

    hal::util::Result<> QuadratureEncoderFeedback::reference(pnm::units::Angle position)
    {
        if (!position.isFinite())
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        // Never call the encoder with the state lock held: its callback acquires
        // that lock. Temporarily detach callbacks while the counter keeps running.
        m_encoder->clearSampleCallback();
        const auto count{ m_encoder->position() };
        hal::util::Result<> result;
        if (count) {
            const auto offset{ position - m_state->count_angle * static_cast<double>(*count) };
            if (!offset.isFinite())
                result = std::unexpected(std::make_error_code(std::errc::result_out_of_range));
            else {
                const State::Guard guard{ m_state->mutex };
                m_state->offset = offset;
                m_state->previous.reset();
            }
        }
        else result = std::unexpected(count.error());
        subscribe(); // Publishes with the new coordinate before returning.
        return result;
    }
}
