#pragma once

// The fixture for the end to end Radio tests (the TestRadio*.cpp files): two Radios joined by
// simulated transports, driven by a fake clock. "A" is the ground station (channel master), "B"
// is the rocket. It lives in a header because gtest wants one fixture class for a suite name
// across all files; each file says `using sim::RadioTest;`.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "Mocks/MockRadioListener.h"
#include "Mocks/MockTransport.h"
#include "Radio.h"
#include "Sim/Recorder.h"
#include "Sim/Samples.h"
#include "Sim/SimTransport.h"

namespace sim
{
inline ra::turtleford::RadioConfig TestConfig(bool Master, uint32_t Seed)
{
    ra::turtleford::RadioConfig Config {};
    Config.Seed                  = Seed;
    Config.KeepaliveMs           = 200;
    Config.AckDelayMs            = 50;
    Config.Link.ChannelMaster    = Master;
    Config.Link.LostTimeoutMs    = 1000;
    Config.Link.ConfirmTimeoutMs = 400;
    Config.Link.SearchDwellMs    = 200;
    return Config;
}

// One end: the application's listener and the radio that reports to it. The listener is a mock
// that also records, so tests can wait on and inspect Events, and put EXPECT_CALLs on Listener.
// NiceMock because most tests care about one or two callbacks, not all five.
struct Node
{
    testing::NiceMock<mocks::MockRadioListener> Listener;
    sim::Recorder& Events = Listener.Rec;
    ra::turtleford::Radio R;

    explicit Node(const ra::turtleford::RadioConfig& Config) : R(Listener, Config) {}
};

using Received = sim::Recorder::Received;

class RadioTest : public testing::Test
{
protected:
    void SetUp() override
    {
        SimTransport::Connect(ALora, BLora);
        SimTransport::Connect(ABle, BBle);
        A = std::make_unique<Node>(ConfigA);
        B = std::make_unique<Node>(ConfigB);
    }

    // Most tests use LoRa alone; the routing tests add BLE as a second transport
    void AttachLora()
    {
        ASSERT_TRUE(A->R.AddTransport(Attached(ALora, ALoraTx)));
        ASSERT_TRUE(B->R.AddTransport(Attached(BLora, BLoraTx)));
    }
    void AttachBoth()
    {
        AttachLora();
        ASSERT_TRUE(A->R.AddTransport(Attached(ABle, ABleTx)));
        ASSERT_TRUE(B->R.AddTransport(Attached(BBle, BBleTx)));
    }

    // For a test that counts or orders the calls the radios make on their transports: attach the
    // mocks (each delegating to its simulation) instead of the simulations themselves. Call it
    // before attaching. A mock call on every poll makes the run many times slower, so only the
    // tests that need it ask.
    void ObserveTransports() { m_ObserveTransports = true; }

    void Run(uint32_t Ms)
    {
        for (uint32_t i = 0; i < Ms; ++i)
        {
            ++Now;
            A->R.Poll(Now);
            B->R.Poll(Now);
        }
    }

    bool RunUntil(const std::function<bool()>& Done, uint32_t MaxMs)
    {
        for (uint32_t i = 0; i < MaxMs && !Done(); ++i) { Run(1); }
        return Done();
    }

    // Let keepalives introduce the two ends to each other
    void Settle()
    {
        ASSERT_TRUE(RunUntil(
            [&]
            {
                return A->R.IsLinkUp(ra::turtleford::TransportKind::Lora) &&
                       B->R.IsLinkUp(ra::turtleford::TransportKind::Lora);
            },
            2000));
        Run(100);
    }

    void CutLora(bool Cut) { ALora.Cut = BLora.Cut = Cut; }

    ra::turtleford::RadioConfig ConfigA = TestConfig(true, 111);
    ra::turtleford::RadioConfig ConfigB = TestConfig(false, 222);

    // The simulated links: they carry the messages and have the knobs (cut, loss, queue delay)
    SimTransport ALora {ra::turtleford::TransportKind::Lora, 1};
    SimTransport BLora {ra::turtleford::TransportKind::Lora, 2};
    SimTransport ABle {ra::turtleford::TransportKind::Ble, 3};
    SimTransport BBle {ra::turtleford::TransportKind::Ble, 4};

    // What the radios are attached to under ObserveTransports(): each delegates to its simulation
    testing::NiceMock<mocks::MockTransport> ALoraTx {ALora};
    testing::NiceMock<mocks::MockTransport> BLoraTx {BLora};
    testing::NiceMock<mocks::MockTransport> ABleTx {ABle};
    testing::NiceMock<mocks::MockTransport> BBleTx {BBle};

    std::unique_ptr<Node> A;
    std::unique_ptr<Node> B;
    uint32_t Now = 0;

    // The simulation itself, or its mock when the test asked to observe transports
    ra::turtleford::ITransport& Attached(SimTransport& Sim, mocks::MockTransport& Mock)
    {
        return m_ObserveTransports ? static_cast<ra::turtleford::ITransport&>(Mock) : Sim;
    }

    bool m_ObserveTransports = false;
};
} // namespace sim
