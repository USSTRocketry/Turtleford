// The soak of TestRadioSoak.cpp, over the real LoRa transport: two complete stacks on a simulated
// LoRa air (LoraRig), so frames take airtime, collide, are lost while a radio transmits or has not
// emptied its one receive buffer, are cut into fragments, and the two ends take turns to speak.
// Outages of a few seconds now and then make both ends lose each other and search the channels.
// After chaos the air turns clean and both applications stop sending; then everything must settle
// with the same promises as there (see Sim/SoakApp.h).

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "Sim/LoraRig.h"
#include "Random.h"
#include "Sim/SoakApp.h"

using namespace ra::turtleford;
using namespace ra::type;
using sim::LoraRig;
using sim::RigConfig;
using sim::SoakApp;
using testing::_;

namespace
{
struct LoraSoakCase
{
    uint32_t Seed;
    uint32_t LossPercent;
    uint8_t MaxAttempts;
    uint16_t FrameSize; // 100 cuts telemetry into two fragments, 200 carries every message whole
};

std::string LoraSoakName(const testing::TestParamInfo<LoraSoakCase>& Info)
{
    return "Seed" + std::to_string(Info.param.Seed) + "_Loss" + std::to_string(Info.param.LossPercent) + "_Attempts" +
           std::to_string(Info.param.MaxAttempts) + "_Frame" + std::to_string(Info.param.FrameSize);
}

RadioConfig SoakRadio(bool Master, uint32_t Seed, uint8_t MaxAttempts)
{
    RadioConfig Config {};
    Config.Seed                  = Seed;
    Config.KeepaliveMs           = 1000;
    Config.AckDelayMs            = 100;
    Config.Sender.MaxAttempts    = MaxAttempts;
    Config.Link.ChannelMaster    = Master;
    Config.Link.LostTimeoutMs    = 6000;
    Config.Link.ConfirmTimeoutMs = 3000;
    Config.Link.SearchDwellMs    = 1500;
    return Config;
}

// Airtime is 10 ms + 0.5 ms per byte, as in TestRadioOverLora.cpp
RigConfig SoakRig(const LoraSoakCase& P)
{
    RigConfig Config {};
    Config.Air.Seed         = P.Seed;
    Config.Lora.ChannelsMHz = {915.0f, 916.0f, 917.0f, 918.0f};
    Config.Lora.FrameSize   = P.FrameSize;
    Config.Lora.RoundTripMs = 400;
    Config.RadioA           = SoakRadio(true, P.Seed * 2, P.MaxAttempts);
    Config.RadioB           = SoakRadio(false, P.Seed * 2 + 1, P.MaxAttempts);
    // the ground station and the flight computer each do other work between polls, on clocks of their own
    Config.PollMsA       = 3;
    Config.PollMsB       = 5;
    Config.BootOffsetMsB = 1000 + P.Seed * 37;
    return Config;
}

// The application is what the stack reports to; the recorder keeps seeing everything too
void Wire(LoraRig::End& E, SoakApp& App)
{
    App.Bind(E.R);
    ON_CALL(E.Listener, OnMessage(_, _))
        .WillByDefault(
            [&E, &App](const Message& Msg, const RxInfo& Info)
            {
                E.Events.OnMessage(Msg, Info);
                App.OnMessage(Msg, Info);
            });
    ON_CALL(E.Listener, OnDelivered(_))
        .WillByDefault(
            [&E, &App](Ticket Id)
            {
                E.Events.OnDelivered(Id);
                App.OnDelivered(Id);
            });
    ON_CALL(E.Listener, OnFailed(_))
        .WillByDefault(
            [&E, &App](Ticket Id)
            {
                E.Events.OnFailed(Id);
                App.OnFailed(Id);
            });
    ON_CALL(E.Listener, OnChannelChanged(_, _))
        .WillByDefault(
            [&E, &App](TransportKind Kind, ChannelNumber Channel)
            {
                E.Events.OnChannelChanged(Kind, Channel);
                App.OnChannelChanged(Kind, Channel);
            });
}

class LoraSoakTest : public testing::TestWithParam<LoraSoakCase>
{
protected:
    void SetUp() override
    {
        const auto& P = GetParam();
        Rig           = std::make_unique<LoraRig>(SoakRig(P));
        Ground        = std::make_unique<SoakApp>(P.Seed * 2, true);
        Rocket        = std::make_unique<SoakApp>(P.Seed * 2 + 1, false);
        Wire(Rig->A, *Ground);
        Wire(Rig->B, *Rocket);
        m_Rng = ra::RNG {P.Seed};
    }

    // One millisecond of air time
    void Step(bool Chaos)
    {
        if (Chaos)
        {
            Ground->Act();
            Rocket->Act();

            // now and then nothing gets through for a few seconds (antenna shadow, range)
            if (m_OutageLeftMs == 0 && m_Rng.Value(0, 14999) == 0)
            {
                m_OutageLeftMs = m_Rng.Value<uint32_t>(2000, 8000);
            }
            Rig->Air.Cut = m_OutageLeftMs > 0;
            if (m_OutageLeftMs > 0) { --m_OutageLeftMs; }
        }
        Rig->Run(1);
    }

    bool Settled() const
    {
        const auto& A = Rig->A.R;
        const auto& B = Rig->B.R;
        return A.PendingReliable() == 0 && B.PendingReliable() == 0 && Rig->LinkUp() &&
               A.Channel(TransportKind::Lora) == B.Channel(TransportKind::Lora);
    }

    std::unique_ptr<LoraRig> Rig;
    std::unique_ptr<SoakApp> Ground;
    std::unique_ptr<SoakApp> Rocket;
    ra::RNG m_Rng {0};
    uint32_t m_OutageLeftMs = 0;
};
} // namespace

TEST_P(LoraSoakTest, EveryPromiseHoldsAfterChaosOnceTheAirIsClean)
{
    constexpr uint32_t ChaosMs = 60000;
    constexpr uint32_t CleanMs = 60000;

    Rig->Air.LossPercent = GetParam().LossPercent;
    for (uint32_t i = 0; i < ChaosMs; ++i) { Step(true); }

    // The applications go quiet: from here on only the stack itself may make anything settle
    Rig->Air.LossPercent = 0;
    Rig->Air.Cut         = false;
    Ground->Quiet = Rocket->Quiet = true;
    uint32_t Waited               = 0;
    while (Waited < CleanMs && !Settled())
    {
        Step(false);
        ++Waited;
    }

    const auto& A       = Rig->A.R;
    const auto& B       = Rig->B.R;
    SCOPED_TRACE("ground sent " + std::to_string(Ground->SentId.size()) + ", rocket sent " +
                 std::to_string(Rocket->SentId.size()) + ", given up " + std::to_string(A.Stats().GivenUp) +
                 "/" + std::to_string(B.Stats().GivenUp) + ", retransmissions " +
                 std::to_string(A.Stats().Retransmissions) + "/" + std::to_string(B.Stats().Retransmissions) +
                 ", collisions " + std::to_string(Rig->Air.Stats.LostCollision) + ", settled after " +
                 std::to_string(Waited) + " ms of clean air");

    ASSERT_TRUE(Settled()) << "pending " << A.PendingReliable() << "/" << B.PendingReliable() << ", link "
                           << A.IsLinkUp(TransportKind::Lora) << "/" << B.IsLinkUp(TransportKind::Lora) << ", channel "
                           << A.Channel(TransportKind::Lora) << "/" << B.Channel(TransportKind::Lora);
    SoakApp::CheckDirection("ground to rocket", *Ground, *Rocket);
    SoakApp::CheckDirection("rocket to ground", *Rocket, *Ground);

    // No channel change may be left open for ever: on clean air a new request is accepted once the
    // last one has finished, and then works
    const auto Elsewhere = [&] { return static_cast<ChannelNumber>((A.Channel(TransportKind::Lora) + 1) % 4); };
    ChannelNumber Other  = Elsewhere();
    uint32_t Tried       = 0;
    while (Tried < 30000 && !Rig->A.R.RequestChannel(TransportKind::Lora, Other))
    {
        Step(false);
        ++Tried;
        Other = Elsewhere(); // an earlier request may still move it
    }
    ASSERT_LT(Tried, 30000u) << "a new channel request is still refused after 30 s of clean air";

    Ground->ChannelReports.clear();
    for (uint32_t i = 0; i < 30000 && Ground->ChannelReports.empty(); ++i) { Step(false); }
    EXPECT_THAT(Ground->ChannelReports, testing::ElementsAre(Other)) << "the request on clean air must land";
    EXPECT_EQ(A.Channel(TransportKind::Lora), Other);
    EXPECT_EQ(B.Channel(TransportKind::Lora), Other);
}

// Plenty of attempts: nothing is given up on
INSTANTIATE_TEST_SUITE_P(Persistent,
                         LoraSoakTest,
                         testing::Values(LoraSoakCase {1, 5, 40, 200},
                                         LoraSoakCase {2, 10, 40, 200},
                                         LoraSoakCase {3, 15, 40, 200},
                                         LoraSoakCase {4, 5, 40, 100},
                                         LoraSoakCase {5, 10, 40, 100},
                                         LoraSoakCase {6, 15, 40, 100}),
                         LoraSoakName);

// Default attempts on a bad link: messages are given up on, and the rest must still arrive once each, in order
INSTANTIATE_TEST_SUITE_P(GivingUp,
                         LoraSoakTest,
                         testing::Values(LoraSoakCase {11, 25, 6, 200},
                                         LoraSoakCase {12, 35, 6, 200},
                                         LoraSoakCase {13, 25, 6, 100},
                                         LoraSoakCase {14, 35, 6, 100}),
                         LoraSoakName);
