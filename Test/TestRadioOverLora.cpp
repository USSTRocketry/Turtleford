// End to end tests of the real chain Radio -> LoraTransport -> HAL radio, on both ends, joined by
// a simulated LoRa air (see Sim/SimRadio.h): airtime, blocking send, half duplex, collisions and a
// one frame receive buffer. "A" is the ground station (channel master), "B" is the rocket.
//
// The simulated radio itself is tested in TestSimRadio.cpp, so a failure here can be trusted to
// be about the stack and not the simulation.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "Mocks/MessageMatchers.h"
#include "Radio.h"
#include "Sim/LoraRig.h"
#include "Sim/Samples.h"
#include "Sim/SimRadio.h"

using namespace ra::turtleford;
using ra::type::CommandMsg;
using ra::type::CommandType;
using ra::type::DebugMsg;
using ra::type::StatusMsg;
using ra::type::FlightData;
using ra::type::FlightState;
using sim::LoraRig;
using sim::RadioAir;
using sim::CommandAt;
using sim::LongText;
using sim::QueuedTelemetry;
using sim::RigConfig;
using sim::SampleFlightData;
using sim::SimRadio;
using mocks::ArrivedReliably;
using mocks::ArrivedVia;
using mocks::CarriesFlightData;
using mocks::HasTimestamp;
using mocks::IsCommand;
using mocks::IsDebug;
using mocks::IsFlightState;
using mocks::IsKind;
using mocks::JoinedText;
using mocks::PartsCountingDown;
using testing::AllOf;
using testing::AtLeast;
using testing::Each;
using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::FloatEq;
using testing::IsEmpty;
using testing::Mock;
using testing::Not;
using testing::SizeIs;
using testing::_;

namespace
{
// ---------------------------------------------------------------------------
// Helpers for the stack
// ---------------------------------------------------------------------------

RadioConfig StackConfig(bool Master, uint32_t Seed)
{
    RadioConfig Config {};
    Config.Seed                  = Seed;
    Config.KeepaliveMs           = 0; // quiet air unless a test asks for keepalives
    Config.AckDelayMs            = 100; // longer than a window of small frames (4 x 20 ms) is on air
    Config.Link.ChannelMaster    = Master;
    Config.Link.LostTimeoutMs    = 6000;
    Config.Link.ConfirmTimeoutMs = 3000;
    Config.Link.SearchDwellMs    = 1500;
    return Config;
}

// Airtime is 10 ms + 0.5 ms per byte, so a 100 byte frame is 60 ms and a full 200 byte frame 110 ms.
// A frame out, the ack delay (100) and a short ack back make about 250 ms; 400 leaves room to spare.
RigConfig DefaultRig()
{
    RigConfig Config {};
    Config.Lora.ChannelsMHz = {915.0f, 916.0f, 917.0f};
    Config.Lora.FrameSize   = 200;
    Config.Lora.RoundTripMs = 400;
    Config.RadioA           = StackConfig(true, 111);
    Config.RadioB           = StackConfig(false, 222);
    return Config;
}

// The flight states B sends in the tests that send several: 1, 2, 3, 4, 1, 2 ...
FlightState StateAt(int Index) { return static_cast<FlightState>(1 + Index % 4); }

// Count flight control messages carrying StateAt(0), StateAt(1) ... in that order
std::vector<testing::Matcher<const sim::Recorder::Received&>> StatesInOrder(int Count)
{
    std::vector<testing::Matcher<const sim::Recorder::Received&>> States;
    for (int i = 0; i < Count; ++i) { States.push_back(IsFlightState(StateAt(i))); }
    return States;
}

class RadioOverLoraTest : public testing::Test
{
protected:
    void Start(const RigConfig& Config = DefaultRig()) { Rig = std::make_unique<LoraRig>(Config); }

    LoraRig::End& A() { return Rig->A; }
    LoraRig::End& B() { return Rig->B; }

    // Keepalives let the two ends find each other without any traffic. B's interval is 30% longer:
    // with equal intervals the two ends fall into lockstep and every keepalive collides (see the
    // disabled test at the end).
    static RigConfig WithKeepalives(uint32_t Ms, bool SameInterval = false)
    {
        RigConfig Config            = DefaultRig();
        Config.RadioA.KeepaliveMs   = Ms;
        Config.RadioB.KeepaliveMs   = SameInterval ? Ms : Ms + Ms * 3 / 10;
        Config.BootOffsetMsB        = Ms / 3 + 7; // two boards never boot in the same millisecond
        return Config;
    }

    // For a test that EXPECT_CALLs on what a transport asks of its radio (see RigConfig::ObserveHal)
    static RigConfig ObservingHal(RigConfig Config = DefaultRig())
    {
        Config.ObserveHal = true;
        return Config;
    }

    bool SettleLink()
    {
        const bool Up = Rig->RunUntil([&] { return Rig->LinkUp(); }, 5000);
        Rig->Run(200);
        return Up;
    }

    // One reliable exchange so both ends know each other's epoch. A cold start is noisy: whoever
    // hears an unknown peer answers at once, over the rest of the sender's burst. Tests about a
    // quiet B or a burst from A start from here.
    void Introduce()
    {
        const auto Hello = A().R.Send(CommandMsg {CommandType::Unknown});
        ASSERT_EQ(Hello.Status, SendStatus::Queued);
        ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.DeliveredTickets.empty(); }, 5000));
        Rig->Run(500);

        for (auto* End : {&A(), &B()})
        {
            End->Events.Messages.clear();
            End->Events.DeliveredTickets.clear();
        }
    }

    // Both ends send six reliable messages over each other from the first poll. Every one must
    // still arrive, in order, and neither end may give up on anything.
    void ExpectBothEndsDeliverEverythingDespiteCollisions(const RigConfig& Config)
    {
        Start(Config);
        EXPECT_CALL(A().Listener, OnFailed(_)).Times(0);
        EXPECT_CALL(B().Listener, OnFailed(_)).Times(0);

        std::vector<CommandType> ExpectedA;
        for (int i = 0; i < 6; ++i)
        {
            ASSERT_TRUE(A().R.Send(CommandMsg {CommandAt(i)}));
            ASSERT_TRUE(B().R.Send(StatusMsg {.State = StateAt(i)}));
            ExpectedA.push_back(CommandAt(i));
        }

        ASSERT_TRUE(Rig->RunUntil(
            [&] { return A().Events.DeliveredTickets.size() == 6 && B().Events.DeliveredTickets.size() == 6; }, 120000));

        EXPECT_GT(Rig->Air.Stats.LostCollision, 0u) << "the two ends really did transmit over each other";
        EXPECT_THAT(B().Events.Commands(), ElementsAreArray(ExpectedA));
        EXPECT_THAT(A().Events.Messages, ElementsAreArray(StatesInOrder(6)));
    }

    // On an idle link the keepalives alone must bring the link up, and it must stay up.
    void ExpectIdleLinkComesUpOnceAndStaysUp(const RigConfig& Config)
    {
        Start(Config);
        for (auto* End : {&A(), &B()})
        {
            EXPECT_CALL(End->Listener, OnLinkChanged(TransportKind::Lora, true)).Times(1);
            EXPECT_CALL(End->Listener, OnLinkChanged(_, false)).Times(0); // the link went down
        }

        ASSERT_TRUE(Rig->RunUntil([&] { return Rig->LinkUp(); }, 10000));
        Rig->Run(30000);

        EXPECT_TRUE(Rig->LinkUp());
    }

    std::unique_ptr<LoraRig> Rig;
};

// The two ends' seeds and when B starts, for the tests that depend on how the random parts of
// the timers fall
struct JitterCase
{
    uint32_t SeedA;
    uint32_t SeedB;
    uint32_t StartMsB; // air time of B's first poll
};

void PrintTo(const JitterCase& Case, std::ostream* Out)
{
    *Out << "{SeedA " << Case.SeedA << ", SeedB " << Case.SeedB << ", B starts at " << Case.StartMsB << " ms}";
}

// The two ends' seeds and B's start offset: the original tests' (111, 222, 5) and eleven others.
// A frame is 20 to 60 ms on air here, so every offset is within one airtime of A's first frame.
std::vector<JitterCase> JitterCases()
{
    return {
        {111, 222, 5}, // what the tests above run
        {1, 2, 0},     {1, 2, 17},      {7, 19, 1},        {100, 200, 12},
        {42, 43, 3},   {31337, 4242, 9}, {5, 6, 19},        {1000, 2000, 7},
        {0xDEADBEEF, 0x12345678, 2}, {9, 8, 15},          {77, 78, 11},
    };
}

std::string JitterName(const testing::TestParamInfo<JitterCase>& Info)
{
    return "SeedA" + std::to_string(Info.param.SeedA) + "_SeedB" + std::to_string(Info.param.SeedB) + "_StartB" +
           std::to_string(Info.param.StartMsB) + "ms";
}

// B starts while A's first frame is still on air, with the seeds and offset of Case. Listen slots are
// off unless asked for: A's first message is the last in its queue, so it says "over" and a window follows,
// which holds A's second frame back. A B that starts after that short first frame (offsets 17 and 19 ms)
// would overlap nothing, and these tests need the ends to transmit over each other.
RigConfig CollisionRig(const JitterCase& Case, uint32_t ListenEveryMs = 0)
{
    auto Config                      = DefaultRig();
    Config.Lora.ListenEveryMs        = ListenEveryMs;
    Config.RadioA.Seed               = Case.SeedA;
    Config.RadioB.Seed               = Case.SeedB;
    Config.StartMsB                  = Case.StartMsB;
    Config.RadioA.Sender.MaxAttempts = 40;
    Config.RadioB.Sender.MaxAttempts = 40;
    return Config;
}
} // namespace

// ---------------------------------------------------------------------------
// Best effort and reliable delivery
// ---------------------------------------------------------------------------

TEST_F(RadioOverLoraTest, BestEffortFlightDataArrivesIntact)
{
    Start(ObservingHal());
    const auto Data = SampleFlightData();

    EXPECT_CALL(B().HalMock, send(_)).Times(1); // one message, one frame
    EXPECT_EQ(B().R.Send(Data).Status, SendStatus::Sent);
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.Messages.empty(); }, 1000));

    EXPECT_THAT(A().Events.Messages,
                ElementsAre(AllOf(IsKind(MessageKind::FlightData),
                                  ArrivedVia(TransportKind::Lora),
                                  Not(ArrivedReliably()),
                                  CarriesFlightData(Data))));
}

TEST_F(RadioOverLoraTest, ReliableCommandIsDeliveredAndAcknowledgedExactlyOnce)
{
    Start();

    const auto Result = A().R.Send(CommandMsg {CommandType::CameraOn});
    ASSERT_EQ(Result.Status, SendStatus::Queued);
    EXPECT_CALL(A().Listener, OnDelivered(Result.Id)).Times(1);
    EXPECT_CALL(A().Listener, OnFailed(_)).Times(0);

    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.DeliveredTickets.empty(); }, 3000));
    Rig->Run(2000); // anything duplicated would show up by now

    EXPECT_THAT(B().Events.Messages, ElementsAre(AllOf(IsCommand(CommandType::CameraOn), ArrivedReliably())));
    EXPECT_EQ(A().R.PendingReliable(), 0u);
    EXPECT_EQ(A().R.Stats().Retransmissions, 0u) << "clean air, nothing to repeat";
}

TEST_F(RadioOverLoraTest, ReliableMessageFlowsInBothDirections)
{
    Start();

    const auto ToB = A().R.Send(CommandMsg {CommandType::StartRecording});
    EXPECT_CALL(A().Listener, OnDelivered(ToB.Id)).Times(1);
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.DeliveredTickets.empty(); }, 10000));

    // one after the other: two cold starts at the same instant just collide (see the BothEnds tests)
    const auto ToA = B().R.Send(StatusMsg {.State = FlightState::InFlight});
    EXPECT_CALL(B().Listener, OnDelivered(ToA.Id)).Times(1);
    ASSERT_TRUE(Rig->RunUntil([&] { return !B().Events.DeliveredTickets.empty(); }, 10000));

    EXPECT_THAT(B().Events.Commands(), ElementsAre(CommandType::StartRecording));
    EXPECT_THAT(A().Events.Messages, ElementsAre(IsFlightState(FlightState::InFlight)));
}

TEST_F(RadioOverLoraTest, BurstOfCommandsArrivesInOrderOnCleanAir)
{
    Start();
    ASSERT_NO_FATAL_FAILURE(Introduce());
    std::vector<CommandType> Expected;
    for (int i = 0; i < 12; ++i)
    {
        Expected.push_back(CommandAt(i));
        ASSERT_TRUE(A().R.Send(CommandMsg {CommandAt(i)}));
    }

    ASSERT_TRUE(Rig->RunUntil([&] { return A().Events.DeliveredTickets.size() == 12; }, 20000));

    EXPECT_THAT(B().Events.Commands(), ElementsAreArray(Expected));
    EXPECT_EQ(A().R.Stats().Retransmissions, 0u);
}

// ---------------------------------------------------------------------------
// Fragmentation
// ---------------------------------------------------------------------------

TEST_F(RadioOverLoraTest, MessageLargerThanOneFrameIsFragmentedAndReassembled)
{
    auto Config            = DefaultRig();
    Config.Lora.FrameSize  = 40; // 36 payload bytes per frame
    Start(ObservingHal(Config));
    const auto Data = SampleFlightData();

    EXPECT_CALL(B().HalMock, send(_)).Times(AtLeast(3)); // it did not fit in one or two frames
    ASSERT_EQ(B().R.Send(Data).Status, SendStatus::Sent);
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.Messages.empty(); }, 2000));

    EXPECT_THAT(A().Events.Messages, ElementsAre(CarriesFlightData(Data)));
    EXPECT_EQ(A().Lora.DroppedFrames(), 0u);
}

TEST_F(RadioOverLoraTest, FragmentedReliableMessageIsDeliveredAndAcknowledged)
{
    auto Config            = DefaultRig();
    Config.Lora.FrameSize  = 40;
    Start(Config);
    const std::string Text(MaxDebugTextLength, 'q');

    const auto Result = B().R.Send(DebugMsg {.Status = 4, .Text = Text}, Route {.Mode = Delivery::Reliable});
    ASSERT_EQ(Result.Status, SendStatus::Queued);
    ASSERT_TRUE(Rig->RunUntil([&] { return !B().Events.DeliveredTickets.empty(); }, 10000));

    EXPECT_THAT(A().Events.Messages, ElementsAre(AllOf(IsDebug(4, Text), ArrivedReliably())));
    EXPECT_EQ(B().R.Stats().Retransmissions, 0u);
}

TEST_F(RadioOverLoraTest, FragmentedReliableMessageSurvivesLosingFragmentsExactlyOnce)
{
    auto Config                      = DefaultRig();
    Config.Lora.FrameSize            = 40;
    Config.Lora.RoundTripMs          = 600; // 4 frames of 30 ms out, then the ack
    Config.RadioB.Sender.MaxAttempts = 30;
    Config.Air.Seed                  = 5;
    Start(Config);
    Rig->Air.LossPercent = 20;
    const std::string Text(MaxDebugTextLength, 'q');

    const auto Result = B().R.Send(DebugMsg {.Status = 4, .Text = Text}, Route {.Mode = Delivery::Reliable});
    ASSERT_EQ(Result.Status, SendStatus::Queued);
    ASSERT_TRUE(Rig->RunUntil([&] { return !B().Events.DeliveredTickets.empty(); }, 120000));
    Rig->Run(3000);

    EXPECT_THAT(A().Events.Messages, ElementsAre(IsDebug(4, Text)))
        << "a message missing one fragment must be dropped, not delivered";
    EXPECT_GT(B().R.Stats().Retransmissions, 0u) << "20% loss over many frames should have cost something";
}

TEST_F(RadioOverLoraTest, LongDebugTextArrivesInPartsCountingDownToZero)
{
    Start(ObservingHal());
    ASSERT_NO_FATAL_FAILURE(Introduce());
    const std::string Long = LongText(MaxDebugTextLength * 2 + 50);

    EXPECT_CALL(B().HalMock, send(_)).Times(3); // one frame per part
    ASSERT_EQ(B().R.Send(DebugMsg {.Status = 9, .Text = Long}).Status, SendStatus::Sent);
    ASSERT_TRUE(Rig->RunUntil([&] { return A().Events.Messages.size() == 3; }, 3000));

    EXPECT_THAT(A().Events.Messages, ElementsAreArray(PartsCountingDown(9, 3)));
    EXPECT_THAT(A().Events.Messages, Each(HasTimestamp(A().Events.Messages[0].Info.Timestamp)));
    EXPECT_EQ(JoinedText(A().Events.Messages), Long);
}

TEST_F(RadioOverLoraTest, LongDebugTextOnFragmentingTransportStillJoinsBack)
{
    auto Config           = DefaultRig();
    Config.Lora.FrameSize = 60; // every part is also cut into frames
    Start(Config);
    ASSERT_NO_FATAL_FAILURE(Introduce());
    const std::string Long = LongText(MaxDebugTextLength * 3 + 1);

    ASSERT_EQ(B().R.Send(DebugMsg {.Status = 2, .Text = Long}).Status, SendStatus::Sent);
    ASSERT_TRUE(Rig->RunUntil([&] { return A().Events.Messages.size() == 4; }, 5000));

    EXPECT_THAT(A().Events.Messages, ElementsAreArray(PartsCountingDown(2, 4)));
    EXPECT_EQ(JoinedText(A().Events.Messages), Long);
}

// ---------------------------------------------------------------------------
// Loss, collisions, a slow receiver
// ---------------------------------------------------------------------------

TEST_F(RadioOverLoraTest, ReliableMessagesSurviveRandomFrameLossInOrderWithoutDuplicates)
{
    auto Config                      = DefaultRig();
    Config.RadioA.Sender.MaxAttempts = 40; // this test is about recovery, not about giving up
    Config.Air.Seed                  = 3;
    Start(Config);
    Rig->Air.LossPercent = 25;
    EXPECT_CALL(A().Listener, OnFailed(_)).Times(0);

    constexpr int Count = 25;
    std::vector<CommandType> Expected;
    int Submitted = 0;

    const auto Done = [&]
    {
        // feed commands in as room becomes available
        while (Submitted < Count && A().R.Send(CommandMsg {CommandAt(Submitted)}))
        {
            Expected.push_back(CommandAt(Submitted));
            ++Submitted;
        }
        return A().Events.DeliveredTickets.size() == static_cast<size_t>(Count);
    };

    ASSERT_TRUE(Rig->RunUntil(Done, 400000));
    Rig->Run(3000);

    EXPECT_THAT(B().Events.Commands(), ElementsAreArray(Expected));
    EXPECT_GT(Rig->Air.Stats.LostRandom, 0u);
    EXPECT_GT(A().R.Stats().Retransmissions, 0u);
}

TEST_F(RadioOverLoraTest, DroppedFrameIsRetransmittedAfterTheRoundTripTimeout)
{
    Start();
    int ToDrop = 1;
    Rig->Air.Drop = [&](std::span<const uint8_t>, size_t Sender) { return Sender == A().Hal.Index() && ToDrop-- > 0; };

    const auto Result = A().R.Send(CommandMsg {CommandType::StopRecording});
    EXPECT_CALL(A().Listener, OnDelivered(Result.Id)).Times(1);
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.DeliveredTickets.empty(); }, 5000));

    EXPECT_THAT(B().Events.Commands(), ElementsAre(CommandType::StopRecording));
    EXPECT_EQ(A().R.Stats().Retransmissions, 1u);
}

TEST_F(RadioOverLoraTest, BothEndsSendingAtOnceCollideAndReliableMessagesStillAllArrive)
{
    auto Config   = CollisionRig({.SeedA = 111, .SeedB = 222, .StartMsB = 5}); // B starts while A's first frame is on air
    Config.PollMsB = 37; // and its loop does other work, so its retries do not follow A's to the ms

    ASSERT_NO_FATAL_FAILURE(ExpectBothEndsDeliverEverythingDespiteCollisions(Config));
}

// Two ends that first transmit within one airtime of each other collide. The random part of the
// retransmit timeout is what stops every retry from colliding again.
TEST_F(RadioOverLoraTest, BothEndsSendingWithinOneAirtimeRecoverFromTheCollision)
{
    ASSERT_NO_FATAL_FAILURE(
        ExpectBothEndsDeliverEverythingDespiteCollisions(CollisionRig({.SeedA = 111, .SeedB = 222, .StartMsB = 5})));
}

// The same with the listen slots of the default configuration, which follow each end's first message
TEST_F(RadioOverLoraTest, BothEndsSendingWithinOneAirtimeRecoverFromTheCollisionWithListenSlots)
{
    const auto Slots = LoraTransportConfig {}.ListenEveryMs;

    ASSERT_NO_FATAL_FAILURE(ExpectBothEndsDeliverEverythingDespiteCollisions(
        CollisionRig({.SeedA = 111, .SeedB = 222, .StartMsB = 5}, Slots)));
    ASSERT_NO_FATAL_FAILURE(
        ExpectBothEndsDeliverEverythingDespiteCollisions(CollisionRig({.SeedA = 1, .SeedB = 2, .StartMsB = 12}, Slots)));
}

TEST_F(RadioOverLoraTest, SlowReceiverOverrunsItsBufferAndLosesBestEffortMessages)
{
    auto Config   = DefaultRig();
    Config.PollMsA = 1000; // A looks at its radio once a second
    Start(ObservingHal(Config));

    // B sends four frames back to back; A's one frame buffer holds the first and loses the rest
    EXPECT_CALL(B().HalMock, send(_)).Times(4);
    for (int i = 0; i < 4; ++i) { ASSERT_EQ(B().R.Send(SampleFlightData(), QueuedTelemetry).Status, SendStatus::Sent); }
    Rig->Run(3000);

    EXPECT_GT(A().Hal.Stats.Overruns, 0u);
    EXPECT_THAT(A().Events.Messages, testing::Contains(IsKind(MessageKind::FlightData)).Times(testing::Lt(4)));
    EXPECT_EQ(A().Hal.Stats.Received + A().Hal.Stats.Overruns, B().Hal.Stats.Sent) << "every frame is accounted for";
}

TEST_F(RadioOverLoraTest, SlowReceiverStillGetsEveryReliableMessageInOrder)
{
    auto Config                      = DefaultRig();
    Config.PollMsA                   = 700;
    Config.RadioB.Sender.MaxAttempts = 40;
    Start(Config);

    // B is the fast sender, A the slow receiver
    EXPECT_CALL(B().Listener, OnFailed(_)).Times(0);
    for (int i = 0; i < 6; ++i) { ASSERT_TRUE(B().R.Send(StatusMsg {.State = StateAt(i)})); }

    ASSERT_TRUE(Rig->RunUntil([&] { return B().Events.DeliveredTickets.size() == 6; }, 200000));
    Rig->Run(3000);

    EXPECT_GT(A().Hal.Stats.Overruns, 0u) << "the window sends frames faster than A looks";
    EXPECT_THAT(A().Events.Messages, ElementsAreArray(StatesInOrder(6)));
}

// ---------------------------------------------------------------------------
// Send queue
// ---------------------------------------------------------------------------

TEST_F(RadioOverLoraTest, BackedUpSendQueueDoesNotCauseSpuriousRetransmissions)
{
    Start();
    ASSERT_NO_FATAL_FAILURE(Introduce());

    // About a second of telemetry ahead of the command: it waits far longer than the 400 ms round trip
    int Queued = 0;
    for (; Queued < 15; ++Queued) { ASSERT_EQ(A().R.Send(SampleFlightData(), QueuedTelemetry).Status, SendStatus::Sent); }
    const auto Result = A().R.Send(CommandMsg {CommandType::SdLogOn});
    ASSERT_EQ(Result.Status, SendStatus::Queued);
    Rig->Run(5); // the command is handed to the transport, behind the telemetry
    ASSERT_LT(A().Lora.TxFree(), 3u) << "the queue should still be nearly full";

    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.DeliveredTickets.empty(); }, 10000));
    Rig->Run(1000);

    EXPECT_EQ(A().R.Stats().Retransmissions, 0u) << "the timer must not run while the frame sits in the queue";
    EXPECT_THAT(B().Events.Commands(), ElementsAre(CommandType::SdLogOn));
    EXPECT_THAT(B().Events.Messages, testing::Contains(IsKind(MessageKind::FlightData)).Times(15));
}

TEST_F(RadioOverLoraTest, SendReturnsFullWhenTheTransportQueueIsFullAndWorksAgainOnceItDrains)
{
    Start();
    ASSERT_NO_FATAL_FAILURE(Introduce());

    uint32_t Accepted = 0;
    while (A().R.Send(SampleFlightData(), QueuedTelemetry).Status == SendStatus::Sent) { ++Accepted; }

    EXPECT_EQ(Accepted, config::lora::TxQueueDepth);
    EXPECT_EQ(A().R.Send(SampleFlightData(), QueuedTelemetry).Status, SendStatus::Full);
    EXPECT_EQ(A().Lora.TxFree(), 0u);

    ASSERT_TRUE(Rig->RunUntil([&] { return A().Lora.TxIdle(); }, 10000));
    EXPECT_EQ(A().R.Send(SampleFlightData(), QueuedTelemetry).Status, SendStatus::Sent);

    Rig->Run(1000);
    EXPECT_THAT(B().Events.Messages, testing::Contains(IsKind(MessageKind::FlightData)).Times(Accepted + 1));
}

TEST_F(RadioOverLoraTest, WhenIdleTelemetryNeverFillsTheTransportQueue)
{
    Start();
    ASSERT_NO_FATAL_FAILURE(Introduce());

    // Five new samples per millisecond of the loop, against a radio that needs about 60 ms per frame
    float Altitude       = 0.0f;
    float LastAccepted   = 0.0f;
    uint32_t Refused     = 0;
    while (Rig->Now() < 3000)
    {
        for (int i = 0; i < 5; ++i)
        {
            auto Sample              = SampleFlightData();
            Sample.BMP_Data.Altitude = ++Altitude;
            const auto Status        = B().R.Send(Sample).Status;
            ASSERT_TRUE(Status == SendStatus::Sent || Status == SendStatus::Full);
            if (Status == SendStatus::Sent) { LastAccepted = Altitude; }
            else { ++Refused; }
        }
        Rig->Run(1);
        ASSERT_GE(B().Lora.TxFree(), config::lora::TxQueueDepth - 1) << "at most one frame is ever queued";
    }
    EXPECT_GT(Refused, 1000u) << "far more samples than the air can carry: the newest are refused";

    // Everything accepted goes out, in order: the last thing to arrive is the last one accepted
    Rig->Run(2000);
    ASSERT_THAT(A().Events.Messages, testing::Contains(IsKind(MessageKind::FlightData)).Times(testing::Gt(10)));
    auto Last              = SampleFlightData();
    Last.BMP_Data.Altitude = LastAccepted;
    EXPECT_THAT(A().Events.Messages.back(), CarriesFlightData(Last));
}

TEST_F(RadioOverLoraTest, AbortSentWhileTelemetryFloodsTheRadioIsNotQueuedBehindIt)
{
    Start();
    ASSERT_NO_FATAL_FAILURE(Introduce());

    uint32_t SubmittedMs = 0;
    uint32_t HeardMs     = 0;
    float Altitude       = 0.0f;
    while (Rig->Now() < 3000)
    {
        for (int i = 0; i < 5; ++i)
        {
            auto Sample              = SampleFlightData();
            Sample.BMP_Data.Altitude = ++Altitude;
            const auto Status        = B().R.Send(Sample).Status;
            ASSERT_TRUE(Status == SendStatus::Sent || Status == SendStatus::Full); // far more than the air carries
        }
        if (SubmittedMs == 0 && Rig->Now() >= 1000)
        {
            ASSERT_EQ(B().R.Send(CommandMsg {CommandType::Abort}).Status, SendStatus::Queued);
            SubmittedMs = Rig->Now();
        }
        Rig->Run(1);
        if (SubmittedMs != 0 && HeardMs == 0 && !A().Events.Commands().empty()) { HeardMs = Rig->Now(); }
    }

    ASSERT_NE(HeardMs, 0u) << "the abort never arrived";
    EXPECT_LT(HeardMs - SubmittedMs, 250u) << "one frame on air plus its own airtime, not a queue of telemetry";
    EXPECT_THAT(A().Events.Commands(), ElementsAre(CommandType::Abort));
}

// ---------------------------------------------------------------------------
// Turn taking: a rocket that never stops sending telemetry still hears the ground
// ---------------------------------------------------------------------------

namespace
{
// Time on air of the frames a message becomes, with every link field at its widest
uint32_t AirtimeOf(const RadioAir& Air, Proto_MainMessage Msg, uint16_t FrameSize = 200)
{
    Msg.epoch      = 0x3FFF;
    Msg.peer_epoch = 0x3FFF;
    Msg.has_seq    = true;
    Msg.seq        = std::numeric_limits<uint16_t>::max();
    Msg.has_ack    = true;
    Msg.ack        = std::numeric_limits<uint16_t>::max();
    Msg.sack       = std::numeric_limits<uint16_t>::max();

    const size_t PayloadPerFrame = FrameSize - FragmentHeader::Size;
    uint32_t Total               = 0;
    for (size_t Left = ProtoEncode(Msg, {}); Left > 0;)
    {
        const size_t Chunk = std::min(Left, PayloadPerFrame);
        Total             += Air.AirtimeMs(FragmentHeader::Size + Chunk);
        Left              -= Chunk;
    }
    return Total;
}

class TurnTakingTest : public RadioOverLoraTest
{
protected:
    // From now on this end is handed a new sample after every poll of the rig, as a flight
    // computer that produces data faster than the radio can send it would
    void Flood(LoraRig::End& End) { m_Flooders.push_back({&End, 0.0f}); }
    void StopFlooding() { m_Flooders.clear(); }

    // Run for Ms, or until Until() is true if that is sooner. Returns whether it was.
    bool RunFlooding(uint32_t Ms, const std::function<bool()>& Until = {})
    {
        return Rig->RunUntil(
            [&]
            {
                for (auto& F : m_Flooders)
                {
                    auto Sample              = SampleFlightData();
                    Sample.BMP_Data.Altitude = ++F.Altitude; // each sample is newer than the last
                    F.End->R.Send(Sample);
                }
                return Until && Until();
            },
            Ms);
    }

    // A new rig, B flooding for 10 s: the samples A receives. With or without listen slots, and with
    // the ground sending a command every second. The link is not what is measured, so nobody gives
    // up on anybody in the silence.
    std::vector<float> FloodedSamples(bool Slots, bool CommandEverySecond)
    {
        StopFlooding();
        auto Config                      = DefaultRig();
        Config.RadioA.Link.LostTimeoutMs = 60000;
        Config.RadioB.Link.LostTimeoutMs = 60000;
        if (!Slots) { Config.Lora.ListenEveryMs = 0; }
        Start(Config);
        Introduce();
        Flood(B());

        uint32_t NextCommandMs = Rig->Now() + 1000;
        RunFlooding(10000,
                    [&]
                    {
                        if (CommandEverySecond && Rig->Now() >= NextCommandMs)
                        {
                            A().R.Send(CommandMsg {CommandType::CameraOn});
                            NextCommandMs += 1000;
                        }
                        return false;
                    });
        return Samples(A());
    }

    // Altitudes of the samples End has received, in arrival order
    static std::vector<float> Samples(const LoraRig::End& End)
    {
        std::vector<float> Out;
        for (const auto& M : End.Events.Messages)
        {
            if (M.Kind == MessageKind::FlightData) { Out.push_back(M.Flight.BMP_Data.Altitude); }
        }
        return Out;
    }

private:
    struct Flooder
    {
        LoraRig::End* End;
        float Altitude;
    };
    std::vector<Flooder> m_Flooders;
};

// When the ground submits its command, and how often each end polls. Polling every millisecond is the
// simulation's best case; a real loop polls every few ms (Turtleford: 5 by default), which matters
// because a radio that has just sent only listens again at its next poll.
struct PhaseCase
{
    uint32_t SubmitMs;
    uint32_t PollMsA;
    uint32_t PollMsB;
};

void PrintTo(const PhaseCase& Case, std::ostream* Out)
{
    *Out << "{submit +" << Case.SubmitMs << " ms, polls " << Case.PollMsA << "/" << Case.PollMsB << " ms}";
}

std::string PhaseName(const testing::TestParamInfo<PhaseCase>& Info)
{
    return "Submit" + std::to_string(Info.param.SubmitMs) + "ms_Polls" + std::to_string(Info.param.PollMsA) + "and" +
           std::to_string(Info.param.PollMsB);
}

std::vector<PhaseCase> PhaseCases()
{
    std::vector<PhaseCase> Cases;
    // 13 moments across one ListenEveryMs, for a bare loop and for loops that poll every few ms
    for (const auto Polls : {std::pair<uint32_t, uint32_t> {0, 0}, {5, 7}, {2, 5}})
    {
        for (uint32_t Submit = 0; Submit <= 996; Submit += 83) { Cases.push_back({Submit, Polls.first, Polls.second}); }
    }
    return Cases;
}

class RadioOverLoraPhaseTest : public TurnTakingTest, public testing::WithParamInterface<PhaseCase>
{
};
} // namespace

// The rocket's radio is on air back to back, with a slot for the ground once per ListenEveryMs.
// The ground sends an Abort at a time that has nothing to do with it. The test is repeated with the
// Abort submitted at different moments across one ListenEveryMs, so it cannot pass because of
// where it happens to fall.
TEST_P(RadioOverLoraPhaseTest, CommandFromTheGroundGetsThroughWhileTheRocketFloodsTelemetry)
{
    auto Config           = DefaultRig();
    Config.PollMsA        = GetParam().PollMsA;
    Config.PollMsB        = GetParam().PollMsB;
    Config.BootOffsetMsB  = 137; // two boards never boot in the same millisecond, and equal loops would run in lockstep
    Start(Config);
    ASSERT_NO_FATAL_FAILURE(Introduce());

    // when the rocket began its last frame marked over
    uint32_t LastOverStartMs = 0;
    Rig->Air.Drop            = [&](std::span<const uint8_t> Frame, size_t Sender)
    {
        if (Sender == B().Hal.Index() && (Frame[4] & FragmentHeader::FlagOver) != 0) { LastOverStartMs = Rig->Air.Now(); }
        return false;
    };

    Flood(B());
    RunFlooding(1500); // the ground has heard the rocket talk on its own, and has had a slot offered
    RunFlooding(GetParam().SubmitMs);

    const uint32_t SubmittedMs = Rig->Now();
    const uint32_t PhaseMs     = SubmittedMs - LastOverStartMs;
    EXPECT_CALL(A().Listener, OnFailed(_)).Times(0);
    ASSERT_EQ(A().R.Send(CommandMsg {CommandType::Abort}).Status, SendStatus::Queued);

    uint32_t HeardMs     = 0;
    uint32_t DeliveredMs = 0;
    RunFlooding(10000,
                [&]
                {
                    if (HeardMs == 0 && !B().Events.Commands().empty()) { HeardMs = Rig->Now() - SubmittedMs; }
                    if (DeliveredMs == 0 && !A().Events.DeliveredTickets.empty()) { DeliveredMs = Rig->Now() - SubmittedMs; }
                    return HeardMs != 0 && DeliveredMs != 0;
                });

    // The ground waits for the rocket's next frame marked over and answers it after the guard time. The
    // rocket marks a bulk message at the first message start at least ListenEveryMs after its last
    // over, and message starts are one telemetry frame apart, so the next one ends at most ListenEveryMs
    // and one frame after the last one did. Then the guard, the Abort's own airtime, and 2 ms plus one
    // poll interval for each end's poll. (The wait is shorter by the part of that period already gone,
    // which is what the phases vary.) The Abort is the last in the ground's queue, so it says "over" too
    // and ends the rocket's window at once: the ack leaves with the rocket's next telemetry frame, one
    // guard and one frame later.
    const auto Lora            = DefaultRig().Lora;
    const uint32_t TelemetryMs = AirtimeOf(Rig->Air, PbGen_FlightData(SampleFlightData()));
    const uint32_t CommandMs   = AirtimeOf(Rig->Air, PbGen_CommandMsg(CommandType::Abort));
    const uint32_t Polls       = GetParam().PollMsA + GetParam().PollMsB;
    const uint32_t HeardBound  = Lora.ListenEveryMs + TelemetryMs + Lora.ListenGuardMs + CommandMs + Polls + 2;
    const uint32_t AckBound    = HeardBound + Lora.ListenGuardMs + TelemetryMs + Polls + 2;

    SCOPED_TRACE(testing::Message() << "submitted " << PhaseMs << " ms after the rocket's last marked frame began");
    ASSERT_NE(HeardMs, 0u) << "the abort never reached the rocket";
    EXPECT_LE(HeardMs, HeardBound);
    EXPECT_THAT(B().Events.Commands(), ElementsAre(CommandType::Abort));

    ASSERT_NE(DeliveredMs, 0u) << "the ground never got the ack";
    EXPECT_LE(DeliveredMs, AckBound);
    EXPECT_EQ(A().R.Stats().Retransmissions, 0u) << "first attempt, no repeat";
}

INSTANTIATE_TEST_SUITE_P(SubmitTimes, RadioOverLoraPhaseTest, testing::ValuesIn(PhaseCases()), PhaseName);

TEST_F(TurnTakingTest, BurstOfCommandsFromTheGroundDuringTheFloodArrivesInOrderExactlyOnce)
{
    Start();
    ASSERT_NO_FATAL_FAILURE(Introduce());
    EXPECT_CALL(A().Listener, OnFailed(_)).Times(0);

    Flood(B());
    RunFlooding(1500);

    // all different, so a swapped or repeated one shows
    const std::vector<CommandType> Burst {CommandType::SdLogOff,
                                          CommandType::Abort,
                                          CommandType::CameraOn,
                                          CommandType::StartRecording,
                                          CommandType::CameraOff,
                                          CommandType::StopRecording,
                                          CommandType::SdLogOn,
                                          CommandType::Unknown};
    uint32_t LastRocketMs = Rig->Now();
    uint32_t MaxGapMs     = 0;
    Rig->Air.Drop         = [&](std::span<const uint8_t>, size_t Sender)
    {
        if (Sender == B().Hal.Index())
        {
            MaxGapMs     = std::max(MaxGapMs, Rig->Air.Now() - LastRocketMs);
            LastRocketMs = Rig->Air.Now();
        }
        return false;
    };
    const uint32_t SubmittedMs = Rig->Now();
    for (const auto Command : Burst) { ASSERT_EQ(A().R.Send(CommandMsg {Command}).Status, SendStatus::Queued); }

    std::vector<uint32_t> ArrivedMs;
    uint32_t DeliveredMs = 0;
    ASSERT_TRUE(RunFlooding(30000,
                            [&]
                            {
                                while (ArrivedMs.size() < B().Events.Commands().size()) { ArrivedMs.push_back(Rig->Now() - SubmittedMs); }
                                if (A().Events.DeliveredTickets.size() == Burst.size() && DeliveredMs == 0) { DeliveredMs = Rig->Now() - SubmittedMs; }
                                return DeliveredMs != 0;
                            }));
    RunFlooding(1000); // a duplicate would show up by now

    EXPECT_THAT(B().Events.Commands(), ElementsAreArray(Burst));
    EXPECT_EQ(A().R.Stats().Retransmissions, 0u);

    // several commands go out in one slot, and the rocket is never silent for longer than a frame and
    // a window: the ground's last command says "over" and ends the rocket's window at once
    ASSERT_THAT(ArrivedMs, SizeIs(Burst.size()));
    EXPECT_LT(ArrivedMs[3] - ArrivedMs[0], DefaultRig().Lora.ListenMs / 2);
    const uint32_t TelemetryMs = AirtimeOf(Rig->Air, PbGen_FlightData(SampleFlightData()));
    EXPECT_LE(MaxGapMs, TelemetryMs + DefaultRig().Lora.ListenMs * 5 / 4 + 5);
}

TEST_F(TurnTakingTest, ReliableMessageFromTheFloodingRocketIsDeliveredAndAcknowledgedWithoutRetransmission)
{
    Start();
    ASSERT_NO_FATAL_FAILURE(Introduce());
    Flood(B());
    RunFlooding(1500);

    const uint32_t SubmittedMs = Rig->Now();
    const auto Result          = B().R.Send(StatusMsg {.State = FlightState::InFlight});
    ASSERT_EQ(Result.Status, SendStatus::Queued);
    EXPECT_CALL(B().Listener, OnDelivered(Result.Id)).Times(1);
    EXPECT_CALL(B().Listener, OnFailed(_)).Times(0);

    uint32_t ArrivedMs = 0;
    uint32_t AckedMs   = 0;
    ASSERT_TRUE(RunFlooding(10000,
                            [&]
                            {
                                if (ArrivedMs == 0 && A().Events.Count(MessageKind::Status) > 0) { ArrivedMs = Rig->Now() - SubmittedMs; }
                                if (AckedMs == 0 && !B().Events.DeliveredTickets.empty()) { AckedMs = Rig->Now() - SubmittedMs; }
                                return AckedMs != 0;
                            }));
    RunFlooding(1500); // a duplicate would show up by now

    EXPECT_EQ(A().Events.Count(MessageKind::Status), 1u) << "delivered once, however often it was sent";
    EXPECT_THAT(A().Events.Messages, testing::Contains(IsFlightState(FlightState::InFlight)));
    EXPECT_EQ(B().R.Stats().Retransmissions, 0u);

    // The message is the last in the rocket's queue, so it says "over": it goes out after the rest of the
    // rocket's window (at most 1.25 x ListenMs and a telemetry frame), and the ground answers it, once its
    // ack delay is up and the guard has passed, inside the rocket's window, which the ack ends.
    const auto Lora            = DefaultRig().Lora;
    const uint32_t TelemetryMs = AirtimeOf(Rig->Air, PbGen_FlightData(SampleFlightData()));
    const uint32_t AckDelayMs  = DefaultRig().RadioA.AckDelayMs;
    EXPECT_LE(ArrivedMs, Lora.ListenMs * 5 / 4 + TelemetryMs + 2);
    EXPECT_LE(AckedMs - ArrivedMs, Lora.ListenGuardMs + AckDelayMs + 30);
}

TEST_F(TurnTakingTest, TelemetryRateWithListenSlotsIsNearlyTheRateWithoutThemAndEverySampleIsNewer)
{
    const size_t Back2Back = FloodedSamples(false, false).size();
    const auto Got         = FloodedSamples(true, false);

    EXPECT_GE(static_cast<double>(Got.size()), 0.75 * static_cast<double>(Back2Back));
    EXPECT_LT(Got.size(), Back2Back) << "the slots cost something";
    EXPECT_TRUE(std::adjacent_find(Got.begin(), Got.end(), std::greater_equal<float> {}) == Got.end())
        << "every sample is newer than the one before";
    EXPECT_EQ(A().Lora.DroppedFrames(), 0u);
}

// The slots are the rocket's; the ground's commands in them end its window early and cost it little
TEST_F(TurnTakingTest, TelemetryRateWhileTheGroundSendsACommandEverySecondIsStillNearlyTheRateWithoutSlots)
{
    const size_t Back2Back = FloodedSamples(false, false).size();
    const auto Got         = FloodedSamples(true, true);

    EXPECT_GE(static_cast<double>(Got.size()), 0.75 * static_cast<double>(Back2Back));
    EXPECT_TRUE(std::adjacent_find(Got.begin(), Got.end(), std::greater_equal<float> {}) == Got.end())
        << "every sample is newer than the one before";
    EXPECT_GE(B().Events.Commands().size(), 9u) << "a command a second, ten seconds, all of them got through";
    EXPECT_EQ(A().Lora.DroppedFrames(), 0u);
}

TEST_F(TurnTakingTest, BothEndsFloodingStillGetSomethingThrough)
{
    // Not a real use case: both ends talk all the time and nobody has a gap to speak into.
    auto Config                      = DefaultRig();
    Config.RadioA.Link.LostTimeoutMs = 60000;
    Config.RadioB.Link.LostTimeoutMs = 60000;
    Start(Config);
    ASSERT_NO_FATAL_FAILURE(Introduce());

    Flood(A());
    Flood(B());
    RunFlooding(20000);
    EXPECT_GE(Samples(A()).size(), 3u);
    EXPECT_GE(Samples(B()).size(), 3u);
}

TEST_F(TurnTakingTest, CommandFromTheGroundGetsThroughAFloodOfFragmentedTelemetryWithoutTalkingOverIt)
{
    auto Config           = DefaultRig();
    Config.Lora.FrameSize = 60; // a telemetry message takes several frames
    Start(Config);
    ASSERT_NO_FATAL_FAILURE(Introduce());
    EXPECT_CALL(A().Listener, OnFailed(_)).Times(0);

    const uint32_t PayloadBytes = 60 - FragmentHeader::Size;
    const size_t Fragments      = (ProtoEncode(PbGen_FlightData(SampleFlightData()), {}) + PayloadBytes - 1) / PayloadBytes;
    ASSERT_GE(Fragments, 2u) << "the test is about messages in several frames";
    ASSERT_LE(Fragments, 3u);

    // The ground must never start a frame between two fragments of one rocket message
    bool RocketMidMessage     = false;
    uint32_t TalkedOverCount = 0;
    Rig->Air.Drop            = [&](std::span<const uint8_t> Frame, size_t Sender)
    {
        const auto Header = FragmentHeader::Parse(std::as_bytes(Frame));
        if (Sender == B().Hal.Index() && Header) { RocketMidMessage = !Header->IsLast(); }
        if (Sender == A().Hal.Index() && RocketMidMessage) { ++TalkedOverCount; }
        return false;
    };

    const uint32_t FloodStartMs = Rig->Now();
    Flood(B());
    RunFlooding(1500);

    // Commands at different moments in the rocket's cycle, one at a time
    const std::vector<CommandType> Commands {
        CommandType::Abort, CommandType::CameraOn, CommandType::StartRecording, CommandType::SdLogOn, CommandType::CameraOff};
    for (size_t i = 0; i < Commands.size(); ++i)
    {
        RunFlooding(300 + 137 * static_cast<uint32_t>(i));
        ASSERT_EQ(A().R.Send(CommandMsg {Commands[i]}).Status, SendStatus::Queued);
        ASSERT_TRUE(RunFlooding(5000, [&] { return A().Events.DeliveredTickets.size() == i + 1; })) << "command " << i;
    }
    RunFlooding(1000);

    EXPECT_THAT(B().Events.Commands(), ElementsAreArray(Commands));
    EXPECT_EQ(A().R.Stats().Retransmissions, 0u);

    EXPECT_EQ(TalkedOverCount, 0u);
    EXPECT_EQ(Rig->Air.Stats.LostCollision, 0u);
    EXPECT_EQ(A().Lora.DroppedFrames(), 0u) << "no rocket message lost a fragment";
    EXPECT_EQ(A().R.Stats().DecodeErrors, 0u);
    // and the rocket kept sending at least three messages in four of what back to back frames would give
    const uint32_t MessageMs = AirtimeOf(Rig->Air, PbGen_FlightData(SampleFlightData()), Config.Lora.FrameSize);
    EXPECT_GE(static_cast<double>(Samples(A()).size()), 0.75 * (Rig->Now() - FloodStartMs) / MessageMs);
}

class RadioOverLoraLossTest : public TurnTakingTest, public testing::WithParamInterface<uint32_t>
{
};

TEST_P(RadioOverLoraLossTest, CommandsFromTheGroundSurviveFrameLossWhileTheRocketFloods)
{
    auto Config     = DefaultRig();
    Config.Air.Seed = GetParam();
    Start(Config);
    ASSERT_NO_FATAL_FAILURE(Introduce());
    Rig->Air.LossPercent = 10;
    EXPECT_CALL(A().Listener, OnFailed(_)).Times(0);

    Flood(B());
    RunFlooding(1500);

    const std::vector<CommandType> Burst {CommandType::SdLogOff,
                                          CommandType::Abort,
                                          CommandType::CameraOn,
                                          CommandType::StartRecording,
                                          CommandType::CameraOff,
                                          CommandType::StopRecording,
                                          CommandType::SdLogOn,
                                          CommandType::Unknown};
    for (const auto Command : Burst) { ASSERT_EQ(A().R.Send(CommandMsg {Command}).Status, SendStatus::Queued); }

    ASSERT_TRUE(RunFlooding(90000, [&] { return A().Events.DeliveredTickets.size() == Burst.size(); }));
    RunFlooding(2000);

    EXPECT_THAT(B().Events.Commands(), ElementsAreArray(Burst));
    EXPECT_GT(Rig->Air.Stats.LostRandom, 0u);
}

INSTANTIATE_TEST_SUITE_P(Seeds, RadioOverLoraLossTest, testing::Values(1u, 2u, 3u, 4u, 5u));

TEST_F(TurnTakingTest, WithoutListenSlotsTheRocketStillSendsBackToBack)
{
    auto Config                = DefaultRig();
    Config.Lora.ListenEveryMs = 0;
    Start(Config);
    ASSERT_NO_FATAL_FAILURE(Introduce());

    Flood(B());
    RunFlooding(3000);

    // one frame right after the other, so the radio is on air nearly all the time
    const uint32_t TelemetryMs = AirtimeOf(Rig->Air, PbGen_FlightData(SampleFlightData()));
    EXPECT_GE(static_cast<double>(Samples(A()).size()), 0.9 * 3000.0 / TelemetryMs);
}

// ---------------------------------------------------------------------------
// Link, keepalives, channels
// ---------------------------------------------------------------------------

TEST_F(RadioOverLoraTest, KeepalivesBringTheLinkUpAndHoldItWhileIdle)
{
    // up exactly once each, never flapped
    ASSERT_NO_FATAL_FAILURE(ExpectIdleLinkComesUpOnceAndStaysUp(WithKeepalives(1000)));

    EXPECT_EQ(A().R.Stats().GivenUp, 0u);
}

TEST_F(RadioOverLoraTest, ReliableTrafficWorksWithKeepalivesRunning)
{
    Start(WithKeepalives(300));
    EXPECT_CALL(A().Listener, OnFailed(_)).Times(0);
    ASSERT_TRUE(SettleLink());

    std::vector<CommandType> Expected;
    for (int i = 0; i < 8; ++i)
    {
        Expected.push_back(CommandAt(i));
        ASSERT_TRUE(A().R.Send(CommandMsg {CommandAt(i)}));
    }

    ASSERT_TRUE(Rig->RunUntil([&] { return A().Events.DeliveredTickets.size() == 8; }, 60000));
    EXPECT_THAT(B().Events.Commands(), ElementsAreArray(Expected));
}

TEST_F(RadioOverLoraTest, ChannelChangeMovesBothRadiosAndTrafficFlowsAfterwards)
{
    Start(ObservingHal(WithKeepalives(500)));
    EXPECT_CALL(A().Listener, OnFailed(_)).Times(0);
    ASSERT_TRUE(SettleLink());
    ASSERT_EQ(A().Hal.Frequency(), 915.0f);
    ASSERT_EQ(B().Hal.Frequency(), 915.0f);

    // each radio is retuned once, straight to the new channel
    EXPECT_CALL(A().HalMock, setFrequency(FloatEq(917.0f))).Times(1);
    EXPECT_CALL(B().HalMock, setFrequency(FloatEq(917.0f))).Times(1);
    ASSERT_TRUE(A().R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(Rig->RunUntil([&] { return A().Hal.Frequency() == 917.0f && B().Hal.Frequency() == 917.0f; }, 20000));
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.ChannelChanges.empty() && !B().Events.ChannelChanges.empty(); },
                              20000));

    EXPECT_EQ(A().R.Channel(TransportKind::Lora), 2u);
    EXPECT_EQ(B().R.Channel(TransportKind::Lora), 2u);
    EXPECT_EQ(A().Events.ChannelChanges.back().second, 2u);
    EXPECT_EQ(B().Events.ChannelChanges.back().second, 2u);

    // and it is a working channel, both ways
    ASSERT_TRUE(A().R.Send(CommandMsg {CommandType::CameraOff}));
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.DeliveredTickets.empty(); }, 20000));
    EXPECT_EQ(B().Events.Commands().back(), CommandType::CameraOff);

    ASSERT_TRUE(B().R.Send(SampleFlightData()));
    ASSERT_TRUE(Rig->RunUntil([&] { return A().Events.Count(MessageKind::FlightData) > 0; }, 20000));
}

TEST_F(RadioOverLoraTest, OnlyTheMasterCanRequestAChannelChange)
{
    Start(ObservingHal(WithKeepalives(500)));
    ASSERT_TRUE(SettleLink());

    EXPECT_CALL(A().HalMock, setFrequency(_)).Times(0);
    EXPECT_CALL(B().HalMock, setFrequency(_)).Times(0);
    EXPECT_FALSE(B().R.RequestChannel(TransportKind::Lora, 1));
    Rig->Run(2000);
    EXPECT_EQ(A().Hal.Frequency(), 915.0f);
    EXPECT_EQ(B().Hal.Frequency(), 915.0f);
}

// The rocket hears the request and retunes, but none of its frames reach the ground until it has
// moved: the ground never gets the ack and stays behind. The two ends search; the master's search
// includes the channel it asked for, where the rocket already is, so they meet there and the ground
// reports the wanted channel once.
TEST_F(RadioOverLoraTest, LostAckOfAChannelRequestStillEndsWithBothRadiosOnTheWantedFrequency)
{
    Start(WithKeepalives(500));
    EXPECT_CALL(A().Listener, OnFailed(_)).Times(0);
    ASSERT_TRUE(SettleLink());

    constexpr size_t Rocket = 1; // B, the second radio attached to the air
    bool LoseRocketFrames   = true;
    Rig->Air.Drop           = [&](std::span<const uint8_t>, size_t Sender) { return LoseRocketFrames && Sender == Rocket; };

    ASSERT_TRUE(A().R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(Rig->RunUntil([&] { return B().Hal.Frequency() == 917.0f; }, 20000)) << "the rocket heard the request";
    EXPECT_EQ(A().Hal.Frequency(), 915.0f) << "the ground never got the ack";
    Rig->Run(200);
    LoseRocketFrames = false;

    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.ChannelChanges.empty(); }, 60000));
    EXPECT_THAT(A().Events.ChannelChanges, ElementsAre(testing::Pair(TransportKind::Lora, ChannelNumber {2})));
    ASSERT_TRUE(Rig->RunUntil([&] { return B().Hal.Frequency() == 917.0f && Rig->LinkUp(); }, 20000));
    Rig->Run(3000); // and it must stay that way

    EXPECT_EQ(A().Hal.Frequency(), 917.0f);
    EXPECT_EQ(B().Hal.Frequency(), 917.0f);
    EXPECT_EQ(A().R.Channel(TransportKind::Lora), 2u);
    EXPECT_EQ(B().R.Channel(TransportKind::Lora), 2u);

    ASSERT_TRUE(A().R.Send(CommandMsg {CommandType::CameraOff}));
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.DeliveredTickets.empty(); }, 20000));
    EXPECT_EQ(B().Events.Commands().back(), CommandType::CameraOff);
}

// The request gets through and both radios retune, but the new frequency carries nothing at first.
// Neither end hears the other there, so both search and can only meet again on the old frequency.
// The master then asks again by itself (RadioConfig::ChannelRequestRetries), and once the new
// frequency works both end there. The ground reports only that final outcome; the rocket reports
// each place it found the ground.
TEST_F(RadioOverLoraTest, BadNewChannelFallsBackAndTheMasterRetriesUntilBothRadiosAreOnTheWantedFrequency)
{
    Start(WithKeepalives(500));
    EXPECT_CALL(A().Listener, OnFailed(_)).Times(0);
    ASSERT_TRUE(SettleLink());

    // Radios are attached to the air in the order A, B
    const std::array<SimRadio*, 2> Senders {&A().Hal, &B().Hal};
    bool BadChannel = true;
    Rig->Air.Drop   = [&](std::span<const uint8_t>, size_t Sender) { return BadChannel && Senders[Sender]->Frequency() == 917.0f; };

    ASSERT_TRUE(A().R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(Rig->RunUntil([&] { return A().Hal.Frequency() == 917.0f && B().Hal.Frequency() == 917.0f; }, 20000))
        << "the request got through and both moved";

    // Both search and can only meet on the old channel. The rocket reports that; the ground does not.
    ASSERT_TRUE(Rig->RunUntil([&] { return !B().Events.ChannelChanges.empty(); }, 60000));
    EXPECT_THAT(B().Events.ChannelChanges, ElementsAre(testing::Pair(TransportKind::Lora, ChannelNumber {0})));
    EXPECT_THAT(A().Events.ChannelChanges, IsEmpty()) << "an attempt that fell back is not reported";
    EXPECT_FALSE(A().R.RequestChannel(TransportKind::Lora, 1)) << "the request is still being worked on";

    // The new frequency works from now on. Nobody calls RequestChannel again.
    BadChannel          = false;
    const auto OnWanted = testing::Pair(TransportKind::Lora, ChannelNumber {2});
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.ChannelChanges.empty(); }, 60000));
    ASSERT_TRUE(Rig->RunUntil([&] { return B().Events.ChannelChanges.back() == std::pair {TransportKind::Lora, ChannelNumber {2}} && Rig->LinkUp(); },
                              20000));
    const size_t RocketReports = B().Events.ChannelChanges.size();
    Rig->Run(3000); // and it must stay that way, with no further reports

    EXPECT_THAT(A().Events.ChannelChanges, ElementsAre(OnWanted));
    // The rocket reports every time it finds the ground again: on the old channel (once or more,
    // the two ends may lose each other again while both search), then on the new one
    ASSERT_EQ(B().Events.ChannelChanges.size(), RocketReports);
    EXPECT_THAT(B().Events.ChannelChanges.back(), OnWanted);
    EXPECT_THAT(std::vector(B().Events.ChannelChanges.begin(), B().Events.ChannelChanges.end() - 1),
                Each(testing::Pair(TransportKind::Lora, ChannelNumber {0})));
    EXPECT_EQ(A().Hal.Frequency(), 917.0f);
    EXPECT_EQ(B().Hal.Frequency(), 917.0f);
}

// What an application gets when it does not set LoraTransportConfig::ChannelsMHz: the one channel
// of the default table (config::lora::ChannelsMHz). Channel control is off: a request is refused,
// and losing the peer never retunes the radio, it just waits on the one channel.
TEST_F(RadioOverLoraTest, DefaultSingleChannelTableHasNoChannelControl)
{
    RigConfig Config        = WithKeepalives(500);
    Config.Lora.ChannelsMHz = LoraTransportConfig {}.ChannelsMHz;
    Start(ObservingHal(Config));
    ASSERT_EQ(A().Lora.Caps().ChannelCount, 1u);
    ASSERT_EQ(A().Hal.Frequency(), config::lora::ChannelsMHz[0]);
    ASSERT_EQ(B().Hal.Frequency(), config::lora::ChannelsMHz[0]);
    ASSERT_TRUE(SettleLink());

    EXPECT_CALL(A().HalMock, setFrequency(_)).Times(0);
    EXPECT_CALL(B().HalMock, setFrequency(_)).Times(0);
    EXPECT_FALSE(A().R.RequestChannel(TransportKind::Lora, 1)) << "no such channel";
    EXPECT_FALSE(A().R.RequestChannel(TransportKind::Lora, 0)) << "already there";

    // An outage long enough for both ends to lose each other and, with channels, start searching
    Rig->Air.Cut = true;
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().R.IsLinkUp(TransportKind::Lora) && !B().R.IsLinkUp(TransportKind::Lora); },
                              20000));
    Rig->Run(10000);
    Rig->Air.Cut = false;
    ASSERT_TRUE(Rig->RunUntil([&] { return Rig->LinkUp(); }, 10000));

    EXPECT_EQ(A().Hal.Frequency(), config::lora::ChannelsMHz[0]);
    EXPECT_EQ(B().Hal.Frequency(), config::lora::ChannelsMHz[0]);
    EXPECT_THAT(A().Events.ChannelChanges, IsEmpty());
    EXPECT_THAT(B().Events.ChannelChanges, IsEmpty());
}

TEST_F(RadioOverLoraTest, PeerGoingSilentFailsReliableMessageAndLinkRecoversWhenItReturns)
{
    Start(WithKeepalives(500));
    ASSERT_TRUE(SettleLink());

    Rig->Air.Cut = true;
    const auto Lost = A().R.Send(CommandMsg {CommandType::Abort});
    ASSERT_EQ(Lost.Status, SendStatus::Queued);
    EXPECT_CALL(A().Listener, OnFailed(Lost.Id)).Times(1);
    EXPECT_CALL(A().Listener, OnDelivered(_)).Times(0);

    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.FailedTickets.empty(); }, 60000));
    EXPECT_GE(A().R.Stats().GivenUp, 1u);
    EXPECT_EQ(A().R.PendingReliable(), 0u);
    Mock::VerifyAndClearExpectations(&A().Listener);

    ASSERT_TRUE(Rig->RunUntil([&] { return !A().R.IsLinkUp(TransportKind::Lora); }, 20000)) << "silence is noticed";

    // the peer comes back
    Rig->Air.Cut = false;
    ASSERT_TRUE(SettleLink());

    const auto Again = A().R.Send(CommandMsg {CommandType::CameraOn});
    ASSERT_EQ(Again.Status, SendStatus::Queued);
    EXPECT_CALL(A().Listener, OnDelivered(Again.Id)).Times(1);
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.DeliveredTickets.empty(); }, 30000));
    EXPECT_EQ(B().Events.Commands().back(), CommandType::CameraOn);
}

// B answers A's first keepalive at once, which starts its own timer one airtime behind A's. With
// exactly equal intervals every later keepalive would overlap the other's and be lost; the random
// part of the interval keeps them apart.
TEST_F(RadioOverLoraTest, EqualKeepaliveIntervalsDoNotCollideForeverOnAnIdleLink)
{
    ASSERT_NO_FATAL_FAILURE(ExpectIdleLinkComesUpOnceAndStaysUp(WithKeepalives(1000, true)));
}

// ---------------------------------------------------------------------------
// The two tests above that depend on timing luck, over many seeds and start offsets
// ---------------------------------------------------------------------------

class RadioOverLoraJitterTest : public RadioOverLoraTest, public testing::WithParamInterface<JitterCase>
{
};

TEST_P(RadioOverLoraJitterTest, BothEndsSendingWithinOneAirtimeRecoverFromTheCollision)
{
    ASSERT_NO_FATAL_FAILURE(ExpectBothEndsDeliverEverythingDespiteCollisions(CollisionRig(GetParam())));
}

TEST_P(RadioOverLoraJitterTest, EqualKeepaliveIntervalsDoNotCollideForeverOnAnIdleLink)
{
    auto Config = WithKeepalives(1000, true);
    Config.RadioA.Seed = GetParam().SeedA;
    Config.RadioB.Seed = GetParam().SeedB;
    Config.StartMsB    = GetParam().StartMsB;

    ASSERT_NO_FATAL_FAILURE(ExpectIdleLinkComesUpOnceAndStaysUp(Config));
}

INSTANTIATE_TEST_SUITE_P(SeedsAndStartOffsets, RadioOverLoraJitterTest, testing::ValuesIn(JitterCases()), JitterName);

// ---------------------------------------------------------------------------
// Addresses: each end only talks to the one device it was built for
// ---------------------------------------------------------------------------

namespace
{
// A third device on the same air and frequency, with its own transport
struct Stranger
{
    SimRadio Hal;
    LoraTransport Lora;

    Stranger(RadioAir& Air, uint8_t Address, uint8_t PeerAddress) :
        Hal(Air), Lora(Hal, LoraTransportConfig {.Address = Address, .PeerAddress = PeerAddress})
    {
    }

    // Put one valid, decodable message on air at the current time
    void Shout(RadioAir& Air)
    {
        Proto_MainMessage Msg = PbGen_CommandMsg(CommandType::Abort);
        Msg.epoch             = 1234;

        std::array<std::byte, Proto_MainMessage_size> Buffer {};
        const size_t Size = ProtoEncode(Msg, Buffer);
        ASSERT_GT(Size, 0u);
        ASSERT_TRUE(Lora.Send({Buffer.data(), Size}));
        Lora.Poll(Air.Now());
    }
};
} // namespace

TEST_F(RadioOverLoraTest, EndsAreProgrammedAsMirrorImages)
{
    Start();

    // A is 1 talking to 2, B is 2 talking to 1: traffic flows both ways
    // (one after the other: both at the same instant is the collision case covered above)
    ASSERT_TRUE(A().R.Send(CommandMsg {CommandType::CameraOn}));
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.DeliveredTickets.empty(); }, 20000));
    Rig->Run(1000);
    ASSERT_TRUE(B().R.Send(StatusMsg {.State = FlightState::InFlight}));
    ASSERT_TRUE(Rig->RunUntil([&] { return !B().Events.DeliveredTickets.empty(); }, 20000));

    EXPECT_EQ(Rig->Air.Stats.LostWrongAddress, 0u);
    EXPECT_EQ(A().Lora.DroppedFrames(), 0u);
    EXPECT_EQ(B().Lora.DroppedFrames(), 0u);
}

TEST_F(RadioOverLoraTest, FrameAddressedToSomebodyElseNeverReachesTheStack)
{
    Start();
    Rig->Run(10);

    // device 9 talking to device 8, on our frequency
    Stranger Neighbour {Rig->Air, 9, 8};
    Neighbour.Hal.setFrequency(A().Hal.Frequency());
    EXPECT_CALL(A().Listener, OnMessage(_, _)).Times(0);
    EXPECT_CALL(B().Listener, OnMessage(_, _)).Times(0);
    ASSERT_NO_FATAL_FAILURE(Neighbour.Shout(Rig->Air));
    Rig->Run(500);

    EXPECT_EQ(Rig->Air.Stats.LostWrongAddress, 2u) << "dropped by both radios";
    EXPECT_EQ(A().Lora.DroppedFrames(), 0u) << "the transport never saw it";
    EXPECT_EQ(A().R.Stats().FramesReceived, 0u);
    EXPECT_EQ(B().R.Stats().FramesReceived, 0u);
}

TEST_F(RadioOverLoraTest, FrameFromAnUnknownSenderIsDroppedEvenWhenAddressedToUs)
{
    Start();
    Rig->Run(10);

    // device 9 sends a well formed Abort straight at the rocket (address 2)
    Stranger Intruder {Rig->Air, 9, 2};
    Intruder.Hal.setFrequency(B().Hal.Frequency());
    ASSERT_NO_FATAL_FAILURE(Intruder.Shout(Rig->Air));
    Rig->Run(500);

    EXPECT_EQ(B().Lora.DroppedFrames(), 1u) << "reached the transport, rejected on the sender address";
    EXPECT_THAT(B().Events.Messages, IsEmpty());
    EXPECT_EQ(B().R.Stats().FramesReceived, 0u);

    // and the real ground station still gets through afterwards
    ASSERT_TRUE(A().R.Send(CommandMsg {CommandType::CameraOn}));
    ASSERT_TRUE(Rig->RunUntil([&] { return !A().Events.DeliveredTickets.empty(); }, 20000));
    EXPECT_THAT(B().Events.Commands(), ElementsAre(CommandType::CameraOn));
}
