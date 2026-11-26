// End to end tests of the basics of one link, on two Radios joined by simulated transports
// (see Sim/RadioTest.h): setup, best effort, debug text, held telemetry, link state, peer
// restarts. Reliable delivery, routing and channel changes are in TestRadioReliable.cpp,
// TestRadioRouting.cpp and TestRadioChannel.cpp.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "Mocks/MessageMatchers.h"
#include "Radio.h"
#include "Sim/Frames.h"
#include "Sim/RadioTest.h"
#include "Sim/Recorder.h"
#include "Sim/SimTransport.h"

using namespace ra::turtleford;
using ra::type::CommandMsg;
using ra::type::CommandType;
using ra::type::DebugMsg;
using ra::type::StatusMsg;
using ra::type::FlightState;
using mocks::ArrivedReliably;
using mocks::ArrivedVia;
using mocks::CarriesFlightData;
using mocks::HasChunksLeft;
using mocks::IsCommand;
using mocks::IsDebug;
using mocks::IsFlightState;
using mocks::IsKind;
using mocks::JoinedText;
using mocks::PartsCountingDown;
using sim::SimTransport;
using testing::AllOf;
using testing::Each;
using testing::ElementsAreArray;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::Mock;
using testing::InSequence;
using testing::Not;
using testing::SizeIs;
using mocks::HasTimestamp;
using mocks::MockTransport;
using testing::NiceMock;
using testing::_;
using sim::TestConfig;
using sim::Node;
using sim::SampleFlightData;
using sim::SampleAt;
using sim::QueuedTelemetry;
using sim::LongText;
using sim::RadioTest;

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

TEST_F(RadioTest, AddTransportRejectsDuplicateKind)
{
    SimTransport Second {TransportKind::Lora};

    EXPECT_TRUE(A->R.AddTransport(ALora));
    EXPECT_FALSE(A->R.AddTransport(Second));
    EXPECT_TRUE(A->R.AddTransport(ABle));
}

// Any and None only have a meaning in a route's preference list
TEST_F(RadioTest, AddTransportRejectsAKindThatIsNotATransport)
{
    NiceMock<MockTransport> CallsItselfAny {TransportCaps {.Kind = TransportKind::Any, .MaxMessageSize = 100}};
    NiceMock<MockTransport> CallsItselfNone {TransportCaps {.Kind = TransportKind::None, .MaxMessageSize = 100}};

    EXPECT_FALSE(A->R.AddTransport(CallsItselfAny));
    EXPECT_FALSE(A->R.AddTransport(CallsItselfNone));
}

// Transports are kept in a vector, so adding one can move the others; the radio must not be
// holding on to where they were (it remembers the transport it last heard the peer on)
TEST_F(RadioTest, TransportAddedAfterTrafficLeavesTheLinkWorking)
{
    AttachLora();
    Settle();
    ASSERT_TRUE(A->R.AddTransport(Attached(ABle, ABleTx)));
    ASSERT_TRUE(B->R.AddTransport(Attached(BBle, BBleTx)));

    const auto Result = B->R.Send(CommandMsg {CommandType::Abort});
    ASSERT_EQ(Result.Status, SendStatus::Queued);
    ASSERT_TRUE(RunUntil([&] { return !B->Events.DeliveredTickets.empty(); }, 2000));
    EXPECT_THAT(B->Events.DeliveredTickets, ElementsAre(Result.Id));

    EXPECT_TRUE(RunUntil([&] { return A->R.IsLinkUp(TransportKind::Ble) && B->R.IsLinkUp(TransportKind::Ble); }, 3000));
}

// A listener callback runs inside Poll(), which is working on the transports: adding one there
// is refused instead of moving them under it. Outside Poll() the same call is accepted.
TEST_F(RadioTest, AddTransportIsRefusedFromInsideACallback)
{
    AttachLora();
    std::optional<bool> AddedInCallback;
    ON_CALL(A->Listener, OnLinkChanged(TransportKind::Lora, true))
        .WillByDefault([&](TransportKind, bool) { AddedInCallback = A->R.AddTransport(Attached(ABle, ABleTx)); });

    Settle();

    ASSERT_TRUE(AddedInCallback.has_value()) << "the link never came up";
    EXPECT_FALSE(*AddedInCallback);
    EXPECT_TRUE(A->R.AddTransport(Attached(ABle, ABleTx)));
}

TEST_F(RadioTest, SendWithoutTransportHasNoRoute)
{
    EXPECT_EQ(A->R.Send(CommandMsg {CommandType::Abort}).Status, SendStatus::NoRoute);
    EXPECT_EQ(A->R.Send(SampleFlightData()).Status, SendStatus::NoRoute);
    A->R.Poll(1); // and polling with nothing attached is fine
}

// ---------------------------------------------------------------------------
// Best effort
// ---------------------------------------------------------------------------

TEST_F(RadioTest, BestEffortFlightDataArrivesIntact)
{
    AttachLora();
    const auto Data = SampleFlightData();

    const auto Result = B->R.Send(Data);
    Run(50);

    EXPECT_EQ(Result.Status, SendStatus::Sent);
    EXPECT_THAT(A->Events.Messages,
                ElementsAre(AllOf(IsKind(MessageKind::FlightData),
                                  ArrivedVia(TransportKind::Lora),
                                  Not(ArrivedReliably()),
                                  CarriesFlightData(Data))));
}

// A transport that takes turns needs to know which messages are one of a stream (nothing answers
// them, more are right behind) and which are not (somebody is waiting for what they say)
TEST_F(RadioTest, OnlyWhenIdleTelemetryIsHandedToTheTransportAsBulk)
{
    ObserveTransports();
    AttachLora();
    Settle();

    // what B's transport has been handed since the last call, as {bulk, not bulk}
    const auto Handed = [&]
    {
        const std::pair<uint32_t, uint32_t> Counts {BLoraTx.BulkSends, BLoraTx.PlainSends};
        BLoraTx.BulkSends  = 0;
        BLoraTx.PlainSends = 0;
        return Counts;
    };
    Handed();

    // telemetry on its default WhenIdle route
    EXPECT_EQ(B->R.Send(SampleFlightData()).Status, SendStatus::Sent);
    Run(10);
    EXPECT_EQ(Handed(), (std::pair<uint32_t, uint32_t> {1, 0}));
    EXPECT_TRUE(BLoraTx.LastBulk);

    // a reliable command
    EXPECT_EQ(B->R.Send(CommandMsg {CommandType::Abort}).Status, SendStatus::Queued);
    Run(10);
    EXPECT_EQ(Handed(), (std::pair<uint32_t, uint32_t> {0, 1}));

    // best effort text
    EXPECT_EQ(B->R.Send(DebugMsg {.Status = 1, .Text = "hello"}).Status, SendStatus::Sent);
    Run(10);
    EXPECT_EQ(Handed(), (std::pair<uint32_t, uint32_t> {0, 1}));

    // telemetry sent through a route that queues it in the transport is not held until the transport is idle,
    // so it is not told apart from other messages either
    EXPECT_EQ(B->R.Send(SampleFlightData(), QueuedTelemetry).Status, SendStatus::Sent);
    Run(10);
    EXPECT_EQ(Handed(), (std::pair<uint32_t, uint32_t> {0, 1}));

    // an ack with nothing to ride on: B only has to answer a command of A's
    EXPECT_EQ(A->R.Send(CommandMsg {CommandType::CameraOn}).Status, SendStatus::Queued);
    Run(80);
    EXPECT_EQ(Handed(), (std::pair<uint32_t, uint32_t> {0, 1}));

    // and a keepalive, after a long silence
    Run(400);
    const auto Keepalives = Handed();
    EXPECT_EQ(Keepalives.first, 0u);
    EXPECT_GE(Keepalives.second, 1u);
}

TEST_F(RadioTest, BestEffortIsSentOnceAndNeverReported)
{
    ObserveTransports();
    AttachLora();
    Settle();
    CutLora(true);

    // lost on air and not retried: B hands its transport exactly one message and hears nothing back
    EXPECT_CALL(BLoraTx, Send(_)).Times(1);
    EXPECT_CALL(B->Listener, OnDelivered(_)).Times(0);
    EXPECT_CALL(B->Listener, OnFailed(_)).Times(0);

    EXPECT_EQ(B->R.Send(SampleFlightData()).Status, SendStatus::Sent);
    Run(150); // shorter than the keepalive interval, so only the message itself goes out

    EXPECT_THAT(A->Events.Messages, testing::Contains(IsKind(MessageKind::FlightData)).Times(0));
    EXPECT_EQ(B->R.PendingReliable(), 0u);
}

TEST_F(RadioTest, DebugTextRoundTrips)
{
    AttachLora();

    EXPECT_TRUE(B->R.Send(DebugMsg {.Status = 7, .Text = "igniter continuity ok"}));
    Run(50);

    EXPECT_THAT(A->Events.Messages, ElementsAre(IsDebug(7, "igniter continuity ok")));
}

TEST_F(RadioTest, DebugTextThatFitsIsOneMessageWithNoChunksLeft)
{
    AttachLora();
    const std::string Text(MaxDebugTextLength, 'x');

    EXPECT_EQ(B->R.Send(DebugMsg {.Status = 1, .Text = Text}).Status, SendStatus::Sent);
    Run(50);

    EXPECT_THAT(A->Events.Messages, ElementsAre(AllOf(IsDebug(1, Text), HasChunksLeft(0))));
}

TEST_F(RadioTest, LongDebugTextIsSplitAndCountsDown)
{
    AttachLora();
    Run(300);
    const std::string Long = LongText(MaxDebugTextLength * 2 + 50);

    EXPECT_EQ(B->R.Send(DebugMsg {.Status = 9, .Text = Long}).Status, SendStatus::Sent);
    Run(50);

    ASSERT_THAT(A->Events.Messages, SizeIs(3));
    EXPECT_THAT(A->Events.Messages, ElementsAreArray(PartsCountingDown(9, 3)));
    EXPECT_THAT(A->Events.Messages, Each(HasTimestamp(A->Events.Messages[0].Info.Timestamp)));
    EXPECT_EQ(JoinedText(A->Events.Messages), Long);
}

TEST_F(RadioTest, LongDebugTextIsRefusedWholeWhenTheTransportLacksRoom)
{
    ObserveTransports();
    AttachLora();
    const std::string Long = LongText(MaxDebugTextLength * 3);

    BLora.FreeSlots = 2;
    EXPECT_CALL(BLoraTx, Send(_)).Times(0); // no part may go out when not all fit
    EXPECT_EQ(B->R.Send(DebugMsg {.Status = 1, .Text = Long}).Status, SendStatus::Full);
    Mock::VerifyAndClearExpectations(&BLoraTx);

    BLora.FreeSlots = 3;
    EXPECT_CALL(BLoraTx, Send(_)).Times(3);
    EXPECT_EQ(B->R.Send(DebugMsg {.Status = 1, .Text = Long}).Status, SendStatus::Sent);
}

TEST_F(RadioTest, LongDebugTextSentReliablySurvivesALossyLink)
{
    AttachLora();
    Settle();
    BLora.LossPercent = 30;
    ALora.LossPercent = 30;

    const std::string Long = LongText(MaxDebugTextLength * 4 + 1);
    const auto Result      = B->R.Send(DebugMsg {.Status = 3, .Text = Long}, Route {.Mode = Delivery::Reliable});
    ASSERT_EQ(Result.Status, SendStatus::Queued);
    EXPECT_EQ(B->R.PendingReliable(), 5u);

    ASSERT_TRUE(RunUntil([&] { return B->R.PendingReliable() == 0; }, 20000));

    // the parts' own tickets stay inside the radio: the application hears once, for the whole text
    EXPECT_THAT(B->Events.DeliveredTickets, ElementsAre(Result.Id));
    EXPECT_THAT(B->Events.FailedTickets, IsEmpty());

    EXPECT_THAT(A->Events.Messages, ElementsAreArray(PartsCountingDown(3, 5)));
    EXPECT_THAT(A->Events.Messages, Each(ArrivedReliably()));
    EXPECT_EQ(JoinedText(A->Events.Messages), Long);
}

TEST_F(RadioTest, LongDebugTextSentReliablyIsAllOrNothing)
{
    AttachLora();
    CutLora(true);
    const Route Reliable {.Mode = Delivery::Reliable};

    // more parts than the window can ever hold
    const std::string Huge = LongText(MaxDebugTextLength * (ConfigA.Sender.Capacity + 1));
    EXPECT_EQ(A->R.Send(DebugMsg {.Status = 1, .Text = Huge}, Reliable).Status, SendStatus::TooLarge);

    // room for all but one part
    const std::string Three = LongText(MaxDebugTextLength * 3);
    for (uint16_t i = 0; i < ConfigA.Sender.Capacity - 2; ++i)
    {
        ASSERT_EQ(A->R.Send(CommandMsg {CommandType::CameraOn}).Status, SendStatus::Queued);
    }
    EXPECT_EQ(A->R.Send(DebugMsg {.Status = 1, .Text = Three}, Reliable).Status, SendStatus::Full);
    EXPECT_EQ(A->R.PendingReliable(), static_cast<size_t>(ConfigA.Sender.Capacity - 2));
}

TEST_F(RadioTest, TimestampIsTheSendersClock)
{
    AttachLora();
    Run(500);

    B->R.Send(SampleFlightData());
    Run(50);

    ASSERT_THAT(A->Events.Messages, testing::Contains(IsKind(MessageKind::FlightData)).Times(1));
    EXPECT_EQ(A->Events.Messages.back().Info.Timestamp, 500u);
}

// ---------------------------------------------------------------------------
// Held telemetry (WhenIdle): a sample waits for an idle transport; once config::radio::HeldPerKind are waiting,
// a new one is refused
// ---------------------------------------------------------------------------

TEST_F(RadioTest, SeveralSamplesSentBetweenPollsAllArriveInOrder)
{
    AttachLora();
    Settle();
    A->Events.Messages.clear();

    for (int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(B->R.Send(SampleAt(100.0f + static_cast<float>(i))).Status, SendStatus::Sent);
    }
    Run(50);

    EXPECT_THAT(A->Events.Messages,
                ElementsAre(CarriesFlightData(SampleAt(100.0f)),
                            CarriesFlightData(SampleAt(101.0f)),
                            CarriesFlightData(SampleAt(102.0f))));
}

TEST_F(RadioTest, WhenIdleSampleIsHeldUntilThePollAndKeepsItsSendTime)
{
    ObserveTransports();
    AttachLora();
    Settle();
    Run(50);

    EXPECT_CALL(BLoraTx, Send(_)).Times(0); // nothing reaches the transport inside Send()
    EXPECT_EQ(B->R.Send(SampleFlightData()).Status, SendStatus::Sent);
    Mock::VerifyAndClearExpectations(&BLoraTx);

    const uint32_t SentAt = Now;
    Run(20);
    ASSERT_THAT(A->Events.Messages, testing::Contains(IsKind(MessageKind::FlightData)).Times(1));
    EXPECT_EQ(A->Events.Messages.back().Info.Timestamp, SentAt) << "stamped when sent, not when it went out";
}

TEST_F(RadioTest, WhileTheTransportIsBusySamplesWaitUntilHeldPerKindThenTheNewestIsRefused)
{
    AttachLora();
    Settle();
    A->Events.Messages.clear();

    // Whatever B hands over stays in its queue for a while: the transport is not idle
    BLora.QueueMs = 2000;
    ASSERT_EQ(B->R.Send(SampleFlightData(), QueuedTelemetry).Status, SendStatus::Sent);
    Run(5);

    for (uint32_t i = 0; i < config::radio::HeldPerKind; ++i)
    {
        ASSERT_EQ(B->R.Send(SampleAt(static_cast<float>(i))).Status, SendStatus::Sent) << "send " << i;
        Run(1);
    }
    EXPECT_EQ(B->R.Send(SampleAt(99.0f)).Status, SendStatus::Full)
        << "the newest is refused, nothing waiting is dropped";

    // once the transport is idle again every one that was accepted goes out, in order
    BLora.QueueMs = 0;
    Run(3000);
    std::vector<testing::Matcher<const sim::Recorder::Received&>> Expected {CarriesFlightData(SampleFlightData())};
    for (uint32_t i = 0; i < config::radio::HeldPerKind; ++i)
    {
        Expected.push_back(CarriesFlightData(SampleAt(static_cast<float>(i))));
    }
    EXPECT_THAT(A->Events.Messages, ElementsAreArray(Expected));
}

TEST_F(RadioTest, ReliableCommandSubmittedAfterHeldTelemetryArrivesBeforeIt)
{
    AttachLora();
    Settle();
    A->Events.Messages.clear();

    // B's transport holds each message for 100 ms before it goes on air
    BLora.QueueMs = 100;
    ASSERT_EQ(B->R.Send(SampleAt(1.0f), QueuedTelemetry).Status, SendStatus::Sent); // keeps the transport busy
    Run(5);
    ASSERT_EQ(B->R.Send(SampleAt(2.0f)).Status, SendStatus::Sent); // held: the transport is busy
    Run(20);
    ASSERT_EQ(B->R.Send(CommandMsg {CommandType::Abort}).Status, SendStatus::Queued); // later, but goes first

    ASSERT_TRUE(RunUntil([&] { return A->Events.Messages.size() == 3; }, 3000));
    EXPECT_THAT(A->Events.Messages,
                ElementsAre(CarriesFlightData(SampleAt(1.0f)),
                            IsCommand(CommandType::Abort),
                            CarriesFlightData(SampleAt(2.0f))));
}

TEST_F(RadioTest, SampleSentWithAnExplicitNonWhenIdleRouteKeepsQueueing)
{
    AttachLora();
    Settle();
    A->Events.Messages.clear();

    for (int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(B->R.Send(SampleAt(static_cast<float>(i)), QueuedTelemetry).Status, SendStatus::Sent);
    }
    Run(50);

    EXPECT_THAT(A->Events.Messages,
                ElementsAre(CarriesFlightData(SampleAt(0.0f)),
                            CarriesFlightData(SampleAt(1.0f)),
                            CarriesFlightData(SampleAt(2.0f))));
}

TEST_F(RadioTest, HeldTelemetryCarriesThePendingAckInsteadOfAFrameOfItsOwn)
{
    ObserveTransports();
    AttachLora();
    Settle();

    A->R.Send(CommandMsg {CommandType::SdLogOn});
    ASSERT_TRUE(RunUntil([&] { return !B->Events.Messages.empty(); }, 500));

    // B now owes an ack, well inside the delay after which it would go alone
    EXPECT_CALL(BLoraTx, Send(_)).Times(1);
    EXPECT_CALL(A->Listener, OnDelivered(_)).Times(1);
    ASSERT_EQ(B->R.Send(SampleFlightData()).Status, SendStatus::Sent);
    Run(150);

    EXPECT_THAT(A->Events.Messages, testing::Contains(IsKind(MessageKind::FlightData)).Times(1));
    EXPECT_EQ(A->R.PendingReliable(), 0u);
}

TEST_F(RadioTest, TwoKindsHeldAtOnceTakeTurnsOnABusyTransport)
{
    ConfigB.Routes[static_cast<size_t>(MessageKind::Debug)] = Route {.Mode = Delivery::BestEffort, .WhenIdle = true};
    SetUp();
    AttachLora();
    Settle();
    A->Events.Messages.clear();

    BLora.QueueMs = 100; // one message per 100 ms at most
    for (int i = 0; i < 1000; ++i)
    {
        B->R.Send(SampleAt(static_cast<float>(i))); // refused (Full) while HeldPerKind are waiting
        B->R.Send(DebugMsg {.Status = 1, .Text = "tick"});
        Run(1);
    }
    Run(500);

    const auto Flights = A->Events.Count(MessageKind::FlightData);
    const auto Debugs  = A->Events.Count(MessageKind::Debug);
    EXPECT_GE(Flights, 5u);
    EXPECT_GE(Debugs, 5u);
    EXPECT_LE(Flights > Debugs ? Flights - Debugs : Debugs - Flights, 1u) << "neither kind may starve the other";

    // and they arrive alternately
    const auto& Got = A->Events.Messages;
    for (size_t i = 1; i < Got.size(); ++i) { EXPECT_NE(Got[i].Kind, Got[i - 1].Kind) << "message " << i; }
}

TEST_F(RadioTest, WhenIdleIsIgnoredForDebugTextSplitIntoParts)
{
    AttachLora();
    Settle();
    A->Events.Messages.clear();
    const std::string Long = LongText(MaxDebugTextLength * 2 + 50);
    const Route Latest {.Mode = Delivery::BestEffort, .WhenIdle = true};

    EXPECT_EQ(B->R.Send(DebugMsg {.Status = 9, .Text = Long}, Latest).Status, SendStatus::Sent);
    EXPECT_EQ(B->R.Send(DebugMsg {.Status = 8, .Text = Long}, Latest).Status, SendStatus::Sent);
    Run(100);

    EXPECT_THAT(A->Events.Messages, SizeIs(6)) << "both texts go out whole, none is replaced";
}

TEST_F(RadioTest, WhenIdleIsIgnoredOnAReliableRoute)
{
    AttachLora();
    Settle();

    const Route Reliable {.Mode = Delivery::Reliable, .WhenIdle = true};
    const auto First  = B->R.Send(SampleAt(1.0f), Reliable);
    const auto Second = B->R.Send(SampleAt(2.0f), Reliable);
    EXPECT_EQ(First.Status, SendStatus::Queued);
    EXPECT_EQ(Second.Status, SendStatus::Queued);

    ASSERT_TRUE(RunUntil([&] { return B->Events.DeliveredTickets.size() == 2; }, 2000));
    EXPECT_THAT(A->Events.Messages, ElementsAre(CarriesFlightData(SampleAt(1.0f)), CarriesFlightData(SampleAt(2.0f))));
}

// ---------------------------------------------------------------------------
// Peer restarts
// ---------------------------------------------------------------------------

TEST_F(RadioTest, PeerRebootIsDetectedAndBothDirectionsResume)
{
    AttachLora();
    EXPECT_CALL(A->Listener, OnFailed(_)).Times(0);
    A->R.Send(CommandMsg {CommandType::CameraOn});
    B->R.Send(StatusMsg {.State = FlightState::GroundIdle});
    ASSERT_TRUE(
        RunUntil([&] { return !A->Events.DeliveredTickets.empty() && !B->Events.DeliveredTickets.empty(); }, 3000));

    // the rocket restarts: all its state is gone, its numbering begins at 0 again
    B = std::make_unique<Node>(TestConfig(false, 333));
    ASSERT_TRUE(B->R.AddTransport(Attached(BLora, BLoraTx)));

    A->R.Send(CommandMsg {CommandType::StartRecording});
    B->R.Send(StatusMsg {.State = FlightState::Unarmed});
    ASSERT_TRUE(
        RunUntil([&] { return A->Events.DeliveredTickets.size() == 2 && !B->Events.DeliveredTickets.empty(); }, 5000));

    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandType::StartRecording));
    EXPECT_THAT(A->Events.Messages,
                ElementsAre(IsFlightState(FlightState::GroundIdle), IsFlightState(FlightState::Unarmed)));
}

TEST_F(RadioTest, CommandInFlightSurvivesPeerReboot)
{
    AttachLora();
    Settle();

    // sent, but the rocket dies before hearing it
    CutLora(true);
    const auto Result = A->R.Send(CommandMsg {CommandType::StopRecording});
    Run(150);

    B = std::make_unique<Node>(TestConfig(false, 444));
    ASSERT_TRUE(B->R.AddTransport(Attached(BLora, BLoraTx)));
    CutLora(false);

    EXPECT_CALL(A->Listener, OnDelivered(Result.Id)).Times(1);
    ASSERT_TRUE(RunUntil([&] { return !A->Events.DeliveredTickets.empty(); }, 5000));
    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandType::StopRecording));
}

TEST_F(RadioTest, PeerRebootIsReportedButFirstContactIsNot)
{
    AttachLora();
    Settle();
    EXPECT_THAT(A->Events.PeerRestarts, IsEmpty()) << "hearing the peer for the first time is not a restart";

    B = std::make_unique<Node>(TestConfig(false, 333));
    ASSERT_TRUE(B->R.AddTransport(Attached(BLora, BLoraTx)));
    ASSERT_TRUE(RunUntil([&] { return !A->Events.PeerRestarts.empty(); }, 3000));
    Run(1000);

    EXPECT_THAT(A->Events.PeerRestarts, ElementsAre(TransportKind::Lora)) << "once";
    EXPECT_THAT(B->Events.PeerRestarts, IsEmpty()) << "the rebooted end met the ground for the first time";
}

// A boot counter is a likely seed. Every boot must then get a different epoch, or the peer cannot
// tell that this end rebooted and reads the new session's numbers against the old one's. With an
// epoch drawn at random, 300 boots would very likely repeat one (about 2.7 repeats expected).
TEST_F(RadioTest, ConsecutiveSeedsGiveEveryBootADifferentEpoch)
{
    constexpr uint32_t Boots = 300;
    std::set<uint32_t> Epochs;
    for (uint32_t Seed = 1000; Seed < 1000 + Boots; ++Seed)
    {
        uint32_t Epoch = 0;
        BLora.Drop     = [&Epoch](std::span<const std::byte> Frame)
        {
            const auto Decoded = sim::DecodeFrame(Frame);
            if (Decoded && Epoch == 0) { Epoch = Decoded->epoch; }
            return false; // only looking
        };
        B = std::make_unique<Node>(TestConfig(false, Seed));
        ASSERT_TRUE(B->R.AddTransport(BLora));
        ASSERT_TRUE(RunUntil([&] { return Epoch != 0; }, 1000)) << "seed " << Seed << " sent nothing";
        Epochs.insert(Epoch);
    }
    BLora.Drop = {};

    EXPECT_EQ(Epochs.size(), Boots);
}

// ---------------------------------------------------------------------------
// Link state
// ---------------------------------------------------------------------------

TEST_F(RadioTest, KeepalivesBringTheLinkUpWithoutTraffic)
{
    AttachLora();
    EXPECT_FALSE(A->R.IsLinkUp(TransportKind::Lora));

    // up once, and keepalives are not application messages
    EXPECT_CALL(A->Listener, OnLinkChanged(TransportKind::Lora, true)).Times(1);
    EXPECT_CALL(A->Listener, OnMessage(_, _)).Times(0);

    Run(600);

    EXPECT_TRUE(A->R.IsLinkUp(TransportKind::Lora));
    EXPECT_TRUE(B->R.IsLinkUp(TransportKind::Lora));
}

TEST_F(RadioTest, NoKeepalivesWhenDisabled)
{
    ObserveTransports();
    ConfigA.KeepaliveMs = 0;
    ConfigB.KeepaliveMs = 0;
    SetUp();
    AttachLora();
    EXPECT_CALL(ALoraTx, Send(_)).Times(0);
    EXPECT_CALL(BLoraTx, Send(_)).Times(0);

    Run(3000);

    EXPECT_FALSE(A->R.IsLinkUp(TransportKind::Lora));
}

TEST_F(RadioTest, LinkGoesDownAndComesBack)
{
    AttachLora();

    // up, down, up again, each reported once and in that order
    {
        InSequence Order;
        EXPECT_CALL(A->Listener, OnLinkChanged(TransportKind::Lora, true)).Times(1);
        EXPECT_CALL(A->Listener, OnLinkChanged(TransportKind::Lora, false)).Times(1);
        EXPECT_CALL(A->Listener, OnLinkChanged(TransportKind::Lora, true)).Times(1);
    }
    Settle();

    CutLora(true);
    Run(1500);
    EXPECT_FALSE(A->R.IsLinkUp(TransportKind::Lora));
    EXPECT_FALSE(B->R.IsLinkUp(TransportKind::Lora));

    CutLora(false);
    Run(500);
    EXPECT_TRUE(A->R.IsLinkUp(TransportKind::Lora));
}

TEST_F(RadioTest, GarbageOnAirIsCountedAndIgnored)
{
    AttachLora();
    Settle();

    // something that is not one of our messages lands in A's receiver
    EXPECT_CALL(A->Listener, OnMessage(_, _)).Times(0);
    std::vector<std::byte> Junk(24, std::byte {0xFF});
    BLora.Send(Junk);
    Run(50);

    EXPECT_GE(A->R.Stats().DecodeErrors, 1u);
    EXPECT_TRUE(A->R.IsLinkUp(TransportKind::Lora));
}

// Protobuf has no checksum: these bytes decode (one field MainMessage does not have, which is skipped), but carry
// no epoch, so they are not one of our frames (see ProtoDecodeTest.EmptyOrUnknownFieldsDecodeToAMessageWithNoEpoch)
TEST_F(RadioTest, FrameThatDecodesButHasNoEpochIsIgnored)
{
    AttachLora();
    Settle();
    const auto Before = A->R.Stats();

    EXPECT_CALL(A->Listener, OnMessage(_, _)).Times(0);
    BLora.Send(std::vector<std::byte> {std::byte {0xE8}, std::byte {0xD8}, std::byte {0x56}, std::byte {0x3A}});
    Run(50);

    EXPECT_EQ(A->R.Stats().DecodeErrors, Before.DecodeErrors + 1);
    EXPECT_TRUE(A->R.IsLinkUp(TransportKind::Lora));
}
