#pragma once

// Two complete stacks facing each other over a simulated LoRa air:
//
//   Radio -> LoraTransport -> SimRadio ~~~ RadioAir ~~~ SimRadio -> LoraTransport -> Radio
//
// Each end is polled on its own schedule (PollMs after its previous poll returned) and reads its
// own clock. Because SimRadio::send() blocks on the shared air clock, the rig runs the ends in
// time order: the end whose next poll is due first goes first, with the air's clock set to that
// time. Both ends polling at the same millisecond therefore really do transmit at the same time.
// A, the ground station, polls first on a tie.

#include <cstdint>
#include <functional>
#include <utility>

#include <gmock/gmock.h>

#include "Mocks/MockRadioListener.h"
#include "Mocks/MockTelemetryRadio.h"
#include "Radio.h"
#include "Sim/Recorder.h"
#include "Sim/SimRadio.h"
#include "Transport/LoraTransport.h"

namespace sim
{
struct RigConfig
{
    AirConfig Air {};
    ra::turtleford::LoraTransportConfig Lora {};
    ra::turtleford::RadioConfig RadioA {};
    ra::turtleford::RadioConfig RadioB {};
    // Time the end spends on other work between two polls. 0 is a bare loop: back at once after a
    // blocking send, one poll per millisecond while idle (the clock only ticks in milliseconds).
    uint32_t PollMsA  = 0;
    uint32_t PollMsB  = 0;
    uint32_t StartMsB = 0; // air time of B's first poll
    // What B's millis() reads at air time 0, i.e. when its board booted relative to A's. Without
    // a difference both ends run identical timers on identical clocks and keep transmitting in lockstep.
    uint32_t BootOffsetMsB = 0;
    // Put the mock radio between each transport and its SimRadio, so a test can EXPECT_CALL on what
    // the transport asks of the radio. Every poll then pays for a mock call, which makes a long
    // run many times slower, so it is off unless a test needs it.
    bool ObserveHal = false;
};

class LoraRig
{
public:
    // One end. The mocks delegate to the simulation, so a test can count or order the calls that
    // cross a layer without changing what happens. Listener: what the stack tells the application.
    // HalMock: what the transport asks of the radio, in the path only when RigConfig::ObserveHal.
    struct End
    {
        SimRadio Hal;
        testing::NiceMock<mocks::MockTelemetryRadio> HalMock;
        ra::turtleford::LoraTransport Lora;
        testing::NiceMock<mocks::MockRadioListener> Listener;
        Recorder& Events;
        ra::turtleford::Radio R;

        uint32_t PollMs        = 0;
        uint32_t NextMs        = 0; // air time of this end's next poll
        uint32_t ClockOffsetMs = 0; // its own clock reads air time plus this

        End(RadioAir& Air,
            const ra::turtleford::LoraTransportConfig& LoraConfig,
            const ra::turtleford::RadioConfig& RadioConfig,
            uint32_t PollIntervalMs,
            bool ObserveHal) :
            Hal(Air), HalMock(Hal), Lora(ObserveHal ? static_cast<HAL::ITelemetryRadio&>(HalMock) : Hal, LoraConfig),
            Events(Listener.Rec), R(Listener, RadioConfig), PollMs(PollIntervalMs)
        {
            R.AddTransport(Lora);
        }
    };

    explicit LoraRig(const RigConfig& Config = {}) :
        Air(Config.Air),
        A(Air, Config.Lora, Config.RadioA, Config.PollMsA, Config.ObserveHal),
        B(Air, Mirrored(Config.Lora), Config.RadioB, Config.PollMsB, Config.ObserveHal)
    {
        B.NextMs        = Config.StartMsB;
        B.ClockOffsetMs = Config.BootOffsetMsB;
    }

    /** @brief The other end's settings: same everything, addresses swapped. */
    static ra::turtleford::LoraTransportConfig Mirrored(ra::turtleford::LoraTransportConfig Config)
    {
        std::swap(Config.Address, Config.PeerAddress);
        return Config;
    }

    LoraRig(const LoraRig&)            = delete;
    LoraRig& operator=(const LoraRig&) = delete;

    /** @brief Time everything has been run up to. After a Run the air clock equals it. */
    uint32_t Now() const { return m_NowMs; }

    void Run(uint32_t Ms)
    {
        const uint32_t Target = m_NowMs + Ms;
        while (Step(Target)) {}
        Finish(Target);
    }

    /** @brief Run until Done() is true, checked after every poll, for at most MaxMs. */
    bool RunUntil(const std::function<bool()>& Done, uint32_t MaxMs)
    {
        const uint32_t Target = m_NowMs + MaxMs;
        while (!Done())
        {
            if (!Step(Target))
            {
                Finish(Target);
                return false;
            }
        }
        Air.SetNow(m_NowMs); // stop where it happened, not at the deadline
        return true;
    }

    bool LinkUp() const
    {
        return A.R.IsLinkUp(ra::turtleford::TransportKind::Lora) && B.R.IsLinkUp(ra::turtleford::TransportKind::Lora);
    }

    RadioAir Air;
    End A;
    End B;

private:
    // Poll whichever end is due first, if that is before Target
    bool Step(uint32_t Target)
    {
        End& Next = (B.NextMs < A.NextMs) ? B : A;
        if (Next.NextMs >= Target) { return false; }

        const uint32_t Start = Next.NextMs;
        Air.SetNow(Start);
        m_NowMs = Start;
        Next.R.Poll(Start + Next.ClockOffsetMs);

        // Now() moved on if it sent a frame, and the loop is back as soon as that returns
        Next.NextMs = Air.Now() + Next.PollMs;
        if (Next.NextMs == Start) { Next.NextMs = Start + 1; }
        return true;
    }

    void Finish(uint32_t Target)
    {
        m_NowMs = Target;
        Air.SetNow(Target);
    }

private:
    uint32_t m_NowMs = 0;
};
} // namespace sim
