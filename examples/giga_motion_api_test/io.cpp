// Electrical fixture: actual application/ICC/executor, synthetic external drivers
// and reference switches. D15 is held HIGH regardless of logical enable requests.
#include "io/app/Application.hpp"
#include "common/icc/transport/InterCoreTransport.hpp"
#include "common/motion/OperatingEnvelope.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include <atomic>

// Readable over SWD, independent of logging and dynamic allocation. This probe
// executes from internal SRAM: the region under test must not supply its own
// control flow. It exercises ES0445 2.3.14's byte/half-word FMC read case.
extern "C" {
extern std::uint32_t _etext;
volatile std::uint32_t motion_api_sdram_words{}, motion_api_sdram_byte_errors{},
                      motion_api_sdram_halfword_errors{};
}

namespace {
using namespace pnm::units::literals;
using Level=hal::gpio::Level;

__attribute__((section(".fast.fmc_probe"), noinline))
bool verify_sdram_reads() noexcept {
    const auto entry = reinterpret_cast<std::uintptr_t>(&_etext);
    if (entry < 0xC0400000U || entry >= 0xC0600000U) return true;
    const auto* words = reinterpret_cast<const volatile std::uint32_t*>(0xC0400000U);
    const auto* bytes = reinterpret_cast<const volatile std::uint8_t*>(words);
    const auto* halves = reinterpret_cast<const volatile std::uint16_t*>(words);
    std::uint32_t byte_errors{}, halfword_errors{};
    for (unsigned i = 0; i < 1024; ++i) {
        const auto word = words[i];
        for (unsigned j = 0; j < 4; ++j)
            byte_errors += bytes[4*i+j] != ((word >> (8*j)) & 0xffU);
        for (unsigned j = 0; j < 2; ++j)
            halfword_errors += halves[2*i+j] != ((word >> (16*j)) & 0xffffU);
    }
    motion_api_sdram_byte_errors = byte_errors;
    motion_api_sdram_halfword_errors = halfword_errors;
    motion_api_sdram_words = 1024;
    return byte_errors == 0 && halfword_errors == 0;
}

class Enable final : public hal::IDigitalOutput {
    std::shared_ptr<hal::IDigitalOutput> pin=hal::core::create_steppers_enable_output();
    std::atomic<Level> logical{Level::High};
public:
    Enable(){pin->write(Level::High);}
    Level read() const noexcept override{return logical.load();}
    void write(Level level) noexcept override{pin->write(Level::High);logical=level;}
    void toggle() noexcept override{write(read()==Level::High?Level::Low:Level::High);}
};
class Input final : public hal::IDigitalInput {
    Level level{Level::Low}; EdgeCallback callback;
public:
    Level read() const noexcept override{return level;}
    void setEdgeCallback(EdgeCallback c) noexcept override {
        hal::stm32::InterruptGuard lock; callback=std::move(c);
    }
    void update(bool active) noexcept {
        const auto next=active?Level::High:Level::Low;
        if(next!=level){level=next;if(callback)callback(level);}
    }
};
class Driver final : public hal::device::IStepperDriver {
    Status current;FaultCallback callback;
public:
    std::string_view name() const noexcept override{return "ELECTRICAL FIXTURE; no physical driver";}
    Status status() const noexcept override{return current;}
    hal::util::Result<> initialize() override{current={.ready=true,.error={}};return {};}
    hal::util::Result<> verify() override{return {};}
    void service() override{}
    void setFaultCallback(FaultCallback c) override{callback=std::move(c);}
};
struct Fixture {
    std::array<std::shared_ptr<Input>,3> inputs{std::make_shared<Input>(),std::make_shared<Input>(),std::make_shared<Input>()};
    std::array<std::int64_t,3> physical{};
    std::array<std::uint64_t,3> accounted{};
    std::shared_ptr<hal::IStepGenerator> generator=hal::core::create_stepper_generator();
    std::unique_ptr<io::motion::Controller> create(std::shared_ptr<pnm::log::ISink> sink,std::pmr::memory_resource* memory){
        const io::motion::Controller::Configuration config{{{3_deg,1.8_deg,16},{3_deg,1.8_deg,16},{3_deg,1.8_deg,8}}};
        const io::motion::Controller::Hardware hardware{std::make_shared<Enable>(),generator,{inputs[0],inputs[1],inputs[2]}};
        auto controller=std::make_unique<io::motion::Controller>(config,[](const auto&,const auto&){
          return io::motion::Controller::Drivers{std::make_shared<Driver>(),std::make_shared<Driver>(),std::make_shared<Driver>()};
        },sink,memory,&hardware);
        // Physical pin observation gives an independent direction for the synthetic
        // homing contacts. It is not feedback or evidence of mechanical motion.
        if(!generator->setProgressCallback([this](const hal::step::Status& s) noexcept {
            for(unsigned i=0;i<3;++i){
                if(s.axes[i]==hal::step::State::Running && !s.pulses[i])accounted[i]=0;
                const auto delta=s.pulses[i]-accounted[i];accounted[i]=s.pulses[i];
                const bool forward=i==0?(GPIOJ->IDR&GPIO_PIN_8):i==1?(GPIOD->IDR&GPIO_PIN_13):(GPIOH->IDR&GPIO_PIN_13);
                physical[i]+=forward?std::int64_t(delta):-std::int64_t(delta);
                inputs[i]->update(physical[i]>=32);
            }
        }))throw std::runtime_error("fixture callback unavailable");
        return controller;
    }
};
}
int main(){
    if (!verify_sdram_reads()) hal::panic("M4 SDRAM subword read verification failed");
    Fixture fixture;
    icc::InterCoreTransport transport{hal::core::create_intercore_channel()};
    auto application=std::make_unique<io::Application>(transport,[&](auto sink,auto* memory){return fixture.create(sink,memory);});
    application->installLogSink();application->start();application->join();
    static_cast<void>(fixture.generator->setProgressCallback({}));
}
