// Randomised end-to-end runs of two Radios over lossy, duplicating links, checking the promises
// the stack makes to the application rather than any one scenario.
//
// Two transports with different delays let frames overtake each other, BLE comes and goes, the
// ground asks for channel changes at random, and both applications do what applications do from
// inside the callbacks: answer a message, send the next one when the last was delivered, ask for
// another channel when a change did not land. Then the air turns clean and both applications stop
// sending. With nothing but the stack running, everything has to settle:
//   - every reliable message the Radio accepted got exactly one OnDelivered or OnFailed
//   - OnDelivered only for a message the other application really received
//   - reliable messages arrived exactly once and in order, skipping only ones given up on
//   - both ends are up, on the same channel, and a new channel request is accepted again

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Radio.h"
#include "Random.h"
#include "Sim/SimTransport.h"
#include "Sim/SoakApp.h"

using namespace ra::turtleford;
using namespace ra::type;
using sim::SimTransport;

namespace
{
// One end: the application acting at random (SoakApp), its radio, and the radio's two transports
class SoakEnd final : public IRadioListener
{
public:
    SoakEnd(const RadioConfig& Config, uint32_t Seed, bool Master) :
        App(Seed, Master), Lora(TransportKind::Lora, Seed * 7 + 1), Ble(TransportKind::Ble, Seed * 7 + 2), R(*this, Config)
    {
        App.Bind(R);
        Ble.DelayMs = 3; // faster than LoRa, so a retransmission on BLE can overtake a LoRa frame
    }

    void Attach()
    {
        ASSERT_TRUE(R.AddTransport(Lora));
        ASSERT_TRUE(R.AddTransport(Ble));
    }

    void OnMessage(const Message& Msg, const RxInfo& Info) override { App.OnMessage(Msg, Info); }
    void OnDelivered(Ticket Id) override { App.OnDelivered(Id); }
    void OnFailed(Ticket Id) override { App.OnFailed(Id); }
    void OnChannelChanged(TransportKind Kind, ChannelNumber Channel) override { App.OnChannelChanged(Kind, Channel); }

    sim::SoakApp App;
    SimTransport Lora;
    SimTransport Ble;
    Radio R; // last: it reports to the application from the moment it runs
};

RadioConfig SoakConfig(bool Master, uint32_t Seed, uint8_t MaxAttempts)
{
    RadioConfig Config {};
    Config.Seed                  = Seed;
    Config.KeepaliveMs           = 200;
    Config.AckDelayMs            = 50;
    Config.Sender.MaxAttempts    = MaxAttempts;
    Config.Link.ChannelMaster    = Master;
    Config.Link.LostTimeoutMs    = 1000;
    Config.Link.ConfirmTimeoutMs = 400;
    Config.Link.SearchDwellMs    = 200;
    return Config;
}

struct SoakCase
{
    uint32_t Seed;
    uint32_t LossPercent;
    uint8_t MaxAttempts;
    uint32_t StartMs = 0; // the clock at the start; near the top of uint32_t to run across the wrap
};

std::string SoakName(const testing::TestParamInfo<SoakCase>& Info)
{
    return "Seed" + std::to_string(Info.param.Seed) + "_Loss" + std::to_string(Info.param.LossPercent) +
           "_Attempts" + std::to_string(Info.param.MaxAttempts) + (Info.param.StartMs != 0 ? "_AcrossClockWrap" : "");
}

class RadioSoakTest : public testing::TestWithParam<SoakCase>
{
protected:
    void SetUp() override
    {
        const auto& P = GetParam();
        Ground = std::make_unique<SoakEnd>(SoakConfig(true, P.Seed * 2, P.MaxAttempts), P.Seed * 2, true);
        Rocket = std::make_unique<SoakEnd>(SoakConfig(false, P.Seed * 2 + 1, P.MaxAttempts), P.Seed * 2 + 1, false);
        SimTransport::Connect(Ground->Lora, Rocket->Lora);
        SimTransport::Connect(Ground->Ble, Rocket->Ble);
        Ground->Attach();
        Rocket->Attach();
        m_Rng = ra::RNG {P.Seed};
        Now = P.StartMs;
    }

    void SetAir(uint32_t Loss, uint32_t Dup)
    {
        for (SimTransport* T : {&Ground->Lora, &Rocket->Lora, &Ground->Ble, &Rocket->Ble})
        {
            T->LossPercent = Loss;
            T->DupPercent  = Dup;
        }
    }

    void Step(bool Chaos)
    {
        ++Now;
        if (Chaos)
        {
            Ground->App.Act();
            Rocket->App.Act();
            // BLE drops out and comes back now and then, on either end
            if (m_Rng.Value(0, 1999) == 0) { Ground->Ble.Ready = !Ground->Ble.Ready; }
            if (m_Rng.Value(0, 1999) == 0) { Rocket->Ble.Ready = !Rocket->Ble.Ready; }
        }
        Ground->R.Poll(Now);
        Rocket->R.Poll(Now);
    }

    bool Settled() const
    {
        return Ground->R.PendingReliable() == 0 && Rocket->R.PendingReliable() == 0 &&
               Ground->R.IsLinkUp(TransportKind::Lora) && Rocket->R.IsLinkUp(TransportKind::Lora) &&
               Ground->R.Channel(TransportKind::Lora) == Rocket->R.Channel(TransportKind::Lora);
    }

    std::unique_ptr<SoakEnd> Ground;
    std::unique_ptr<SoakEnd> Rocket;
    uint32_t Now = 0;
    ra::RNG m_Rng {0};
};
} // namespace

TEST_P(RadioSoakTest, EveryPromiseHoldsAfterChaosOnceTheAirIsClean)
{
    constexpr uint32_t ChaosMs = 30000;
    constexpr uint32_t CleanMs = 30000;

    SetAir(GetParam().LossPercent, 5);
    for (uint32_t i = 0; i < ChaosMs; ++i) { Step(true); }

    // The applications go quiet: from here on only the stack itself may make anything settle
    SetAir(0, 0);
    Ground->Ble.Ready = Rocket->Ble.Ready = true;
    Ground->App.Quiet = Rocket->App.Quiet = true;
    uint32_t Waited = 0;
    while (Waited < CleanMs && !Settled())
    {
        Step(false);
        ++Waited;
    }
    Step(false);

    SCOPED_TRACE("ground sent " + std::to_string(Ground->App.SentId.size()) + ", rocket sent " +
                 std::to_string(Rocket->App.SentId.size()) + ", given up " +
                 std::to_string(Ground->R.Stats().GivenUp) + "/" + std::to_string(Rocket->R.Stats().GivenUp) +
                 ", settled after " + std::to_string(Waited) + " ms of clean air");

    EXPECT_TRUE(Settled()) << "pending " << Ground->R.PendingReliable() << "/" << Rocket->R.PendingReliable()
                           << ", link " << Ground->R.IsLinkUp(TransportKind::Lora) << "/"
                           << Rocket->R.IsLinkUp(TransportKind::Lora) << ", channel "
                           << Ground->R.Channel(TransportKind::Lora) << "/"
                           << Rocket->R.Channel(TransportKind::Lora);
    sim::SoakApp::CheckDirection("ground to rocket", Ground->App, Rocket->App);
    sim::SoakApp::CheckDirection("rocket to ground", Rocket->App, Ground->App);

    // No channel change may be left open for ever: on clean air a new request is accepted once the
    // last one has finished, and then works
    const auto Elsewhere = [&] { return static_cast<ChannelNumber>((Ground->R.Channel(TransportKind::Lora) + 1) % 4); };
    ChannelNumber Other  = Elsewhere();
    uint32_t Tried       = 0;
    while (Tried < 20000 && !Ground->R.RequestChannel(TransportKind::Lora, Other))
    {
        Step(false);
        ++Tried;
        Other = Elsewhere(); // an earlier request may still move it
    }
    ASSERT_LT(Tried, 20000u) << "a new channel request is still refused after 20 s of clean air";

    Ground->App.ChannelReports.clear();
    for (uint32_t i = 0; i < 20000 && Ground->App.ChannelReports.empty(); ++i) { Step(false); }
    EXPECT_THAT(Ground->App.ChannelReports, testing::ElementsAre(Other)) << "the request on clean air must land";
    EXPECT_EQ(Ground->R.Channel(TransportKind::Lora), Other);
    EXPECT_EQ(Rocket->R.Channel(TransportKind::Lora), Other);
}

std::vector<SoakCase> Cases(uint32_t FirstSeed,
                            uint32_t Count,
                            std::vector<uint32_t> Losses,
                            uint8_t MaxAttempts,
                            uint32_t StartMs = 0)
{
    std::vector<SoakCase> Out;
    for (uint32_t i = 0; i < Count; ++i)
    {
        Out.push_back({.Seed        = FirstSeed + i,
                       .LossPercent = Losses[i % Losses.size()],
                       .MaxAttempts = MaxAttempts,
                       .StartMs     = StartMs});
    }
    return Out;
}

// Plenty of attempts: nothing is given up on
INSTANTIATE_TEST_SUITE_P(Persistent, RadioSoakTest, testing::ValuesIn(Cases(1, 40, {10, 20, 30}, 40)), SoakName);

// Default attempts on a bad link: messages are given up on, and the rest must still arrive once each, in order
INSTANTIATE_TEST_SUITE_P(GivingUp, RadioSoakTest, testing::ValuesIn(Cases(101, 40, {40, 50, 60}, 6)), SoakName);

// The millisecond clock wraps 49.7 days after boot. Start 20 s before it, so the chaos runs across it.
// This shows nothing stalls or is misreported across the wrap. It does not catch a naive time
// comparison: polled every millisecond, the clock passes every deadline set before the wrap before
// it wraps, so such a bug only makes timers fire early. ArqTimeTest.HandlesClockWrap covers that.
constexpr uint32_t BeforeWrap = std::numeric_limits<uint32_t>::max() - 20000;
INSTANTIATE_TEST_SUITE_P(ClockWrap,
                         RadioSoakTest,
                         testing::ValuesIn([] {
                             auto Out = Cases(201, 3, {10, 20, 30}, 40, BeforeWrap);
                             for (const auto& Case : Cases(301, 3, {40, 50, 60}, 6, BeforeWrap)) { Out.push_back(Case); }
                             return Out;
                         }()),
                         SoakName);
