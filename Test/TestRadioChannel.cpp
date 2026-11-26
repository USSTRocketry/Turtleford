// End to end tests of channel changes and channel request retries, on two Radios joined by
// simulated transports (see Sim/RadioTest.h).

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <span>

#include "Mocks/MessageMatchers.h"
#include "Radio.h"
#include "Sim/Frames.h"
#include "Sim/RadioTest.h"

using namespace ra::turtleford;
using ra::type::CommandMsg;
using ra::type::CommandType;
using ra::type::DebugMsg;
using mocks::IsKind;
using testing::Each;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::Pair;
using testing::SizeIs;
using sim::IsCommandFrame;
using sim::IsSwitchRequest;
using sim::TestConfig;
using sim::Node;
using sim::SampleFlightData;
using sim::CommandAt;
using sim::LongText;
using sim::RadioTest;

// ---------------------------------------------------------------------------
// Channel changes
// ---------------------------------------------------------------------------

class RadioChannelTest : public RadioTest
{
protected:
    bool OnSameChannel() const { return ALora.Channel() == BLora.Channel(); }

    bool Converged()
    {
        return OnSameChannel() && A->R.IsLinkUp(TransportKind::Lora) && B->R.IsLinkUp(TransportKind::Lora) &&
               !A->Events.ChannelChanges.empty();
    }

    // After any channel adventure the link must carry commands again
    void ExpectCommandGetsThrough()
    {
        const auto Before = A->Events.DeliveredTickets.size();
        const auto Heard  = B->Events.Commands().size();

        A->R.Send(CommandMsg {CommandType::CameraOn});

        ASSERT_TRUE(RunUntil([&] { return A->Events.DeliveredTickets.size() == Before + 1; }, 30000));
        EXPECT_EQ(B->Events.Commands().size(), Heard + 1);
    }
};

TEST_F(RadioChannelTest, RequestIsRefusedWhenItCannotWork)
{
    AttachLora();
    Settle();

    EXPECT_FALSE(B->R.RequestChannel(TransportKind::Lora, 2)) << "only the master may ask";
    EXPECT_FALSE(A->R.RequestChannel(TransportKind::Ble, 2)) << "no such transport";
    EXPECT_FALSE(A->R.RequestChannel(TransportKind::Lora, 9)) << "no such channel";
    EXPECT_FALSE(A->R.RequestChannel(TransportKind::Lora, 0)) << "already there";

    EXPECT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    EXPECT_FALSE(A->R.RequestChannel(TransportKind::Lora, 3)) << "one change at a time";
}

// A channel is just a number the transport understands: nothing on the way may cut it to a byte
TEST_F(RadioChannelTest, ChannelNumbersAreNotLimitedToASmallTable)
{
    ALora.SetChannelCount(1000);
    BLora.SetChannelCount(1000);
    AttachLora();
    Settle();

    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 700));
    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty() && !B->Events.ChannelChanges.empty(); }, 5000));

    EXPECT_EQ(ALora.Channel(), 700u);
    EXPECT_EQ(BLora.Channel(), 700u);
    EXPECT_THAT(A->Events.ChannelChanges, ElementsAre(Pair(TransportKind::Lora, ChannelNumber {700})));
    EXPECT_THAT(B->Events.ChannelChanges, ElementsAre(Pair(TransportKind::Lora, ChannelNumber {700})));
}

TEST_F(RadioChannelTest, BothEndsMoveAndReportTheNewChannel)
{
    AttachLora();
    Settle();

    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty() && !B->Events.ChannelChanges.empty(); }, 5000));

    EXPECT_EQ(ALora.Channel(), 2);
    EXPECT_EQ(BLora.Channel(), 2);
    EXPECT_EQ(A->R.Channel(TransportKind::Lora), 2);
    EXPECT_THAT(A->Events.ChannelChanges, ElementsAre(Pair(TransportKind::Lora, ChannelNumber {2})));
    EXPECT_THAT(B->Events.ChannelChanges, ElementsAre(Pair(TransportKind::Lora, ChannelNumber {2})));

    // the request is internal: the application sees neither a message nor a ticket for it
    EXPECT_THAT(B->Events.Messages, IsEmpty());
    EXPECT_THAT(A->Events.DeliveredTickets, IsEmpty());

    ExpectCommandGetsThrough();
}

TEST_F(RadioChannelTest, TrafficKeepsFlowingAcrossAChange)
{
    AttachLora();
    Settle();

    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 3));
    // commands queued behind the change, telemetry streaming throughout
    for (int i = 0; i < 4; ++i) { A->R.Send(CommandMsg {CommandAt(i)}); }
    for (int i = 0; i < 100; ++i)
    {
        B->R.Send(SampleFlightData());
        Run(20);
    }

    EXPECT_EQ(ALora.Channel(), 3);
    EXPECT_EQ(BLora.Channel(), 3);
    EXPECT_THAT(A->Events.DeliveredTickets, SizeIs(4));
    EXPECT_THAT(B->Events.Commands(), SizeIs(4));
    EXPECT_THAT(A->Events.Messages, testing::Contains(IsKind(MessageKind::FlightData)).Times(testing::Gt(50)));
}

TEST_F(RadioChannelTest, SeveralChangesInARow)
{
    AttachLora();
    Settle();

    for (const ChannelNumber Target : {ChannelNumber {1}, ChannelNumber {3}, ChannelNumber {0}, ChannelNumber {2}})
    {
        const auto Seen = A->Events.ChannelChanges.size();
        ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, Target));
        ASSERT_TRUE(RunUntil(
            [&] { return A->Events.ChannelChanges.size() > Seen && B->Events.ChannelChanges.size() > Seen; }, 5000));

        EXPECT_EQ(ALora.Channel(), Target);
        EXPECT_EQ(BLora.Channel(), Target);
        Run(100);
    }

    ExpectCommandGetsThrough();
}

TEST_F(RadioChannelTest, RequestThatNeverArrivesLeavesBothEndsTogether)
{
    AttachLora();
    Settle();

    // the request is lost every time
    ALora.Cut = true;
    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    Run(4000);
    EXPECT_EQ(BLora.Channel(), 0) << "the rocket never heard it";

    ALora.Cut = false;
    ASSERT_TRUE(RunUntil([&] { return Converged(); }, 20000));

    EXPECT_EQ(ALora.Channel(), BLora.Channel());
    ExpectCommandGetsThrough();
}

TEST_F(RadioChannelTest, LostAckOfARequestStillEndsWithBothEndsTogether)
{
    AttachLora();
    Settle();

    // the rocket hears the request and moves, but its ack never reaches the ground
    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    BLora.Cut = true;
    ASSERT_TRUE(RunUntil([&] { return BLora.Channel() == 2; }, 2000));
    EXPECT_EQ(ALora.Channel(), 0) << "the ground does not know";
    Run(200);
    BLora.Cut = false;

    ASSERT_TRUE(RunUntil([&] { return Converged(); }, 30000));
    Run(3000); // and it must stay that way
    EXPECT_EQ(ALora.Channel(), BLora.Channel());
    EXPECT_TRUE(A->R.IsLinkUp(TransportKind::Lora));

    ExpectCommandGetsThrough();
}

TEST_F(RadioChannelTest, RocketRebootOnHomeChannelIsFoundAgain)
{
    AttachLora();
    Settle();
    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty() && !B->Events.ChannelChanges.empty(); }, 5000));
    ASSERT_EQ(BLora.Channel(), 2);

    // the rocket restarts and comes up on its home channel, the ground is still on 2
    BLora.SetChannel(0);
    B = std::make_unique<Node>(TestConfig(false, 555));
    ASSERT_TRUE(B->R.AddTransport(Attached(BLora, BLoraTx)));
    A->Events.ChannelChanges.clear();

    ASSERT_TRUE(RunUntil([&] { return Converged(); }, 30000));

    EXPECT_EQ(ALora.Channel(), 0);
    EXPECT_EQ(BLora.Channel(), 0);
    ExpectCommandGetsThrough();
}

TEST_F(RadioChannelTest, ChangeOnOneTransportLeavesTheOtherAlone)
{
    AttachBoth();
    ASSERT_TRUE(RunUntil(
        [&] { return A->R.IsLinkUp(TransportKind::Lora) && A->R.IsLinkUp(TransportKind::Ble); }, 2000));
    Run(100);

    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 1));
    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty() && !B->Events.ChannelChanges.empty(); }, 5000));

    EXPECT_EQ(ALora.Channel(), 1);
    EXPECT_EQ(BLora.Channel(), 1);
    EXPECT_EQ(ABle.Channel(), 0);
    EXPECT_EQ(BBle.Channel(), 0);
}

// The master holds its transport for a moment while a change waits for the radio to drain. The link is
// still up, so a best effort message is turned away as Full (try again), not as NoRoute (no way out).
TEST_F(RadioChannelTest, BestEffortMessageIsRefusedAsFullWhileARetuneIsHeld)
{
    ObserveTransports();
    AttachLora();
    Settle();

    // the radio never reports itself idle, so the retune keeps waiting after the request is acked
    ON_CALL(ALoraTx, TxIdle()).WillByDefault(testing::Return(false));
    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return A->R.PendingReliable() == 0; }, 5000));
    Run(1);
    ASSERT_EQ(A->R.Channel(TransportKind::Lora), 0) << "the retune is still waiting";
    ASSERT_TRUE(A->R.IsLinkUp(TransportKind::Lora));

    EXPECT_EQ(A->R.Send(DebugMsg {.Status = 1, .Text = "hi"}).Status, SendStatus::Full);
    EXPECT_EQ(A->R.Send(DebugMsg {.Status = 1, .Text = LongText(MaxDebugTextLength * 3)}).Status, SendStatus::Full)
        << "a text in several parts as well";
    EXPECT_EQ(A->R.Send(CommandMsg {CommandType::CameraOn}).Status, SendStatus::Queued) << "reliable ones wait";
}

// ---------------------------------------------------------------------------
// A channel request is a goal: it is asked again until it works or the retries run out
// ---------------------------------------------------------------------------

class RadioChannelRetryTest : public RadioChannelTest
{
protected:
    // Channel 2 carries nothing while BadChannel is set, and the rocket frames are lost while
    // it is still on its old channel and LoseRocketFrames is set. Together: the rocket hears the
    // request and moves, its ack never reaches the ground, and the new channel does not work, so
    // when both ends search they can only meet again on the old one.
    void ArrangeLostAckOnABadChannel()
    {
        ALora.Drop = [this](std::span<const std::byte>) { return BadChannel && ALora.Channel() == 2; };
        BLora.Drop = [this](std::span<const std::byte>)
        { return (BadChannel && BLora.Channel() == 2) || (LoseRocketFrames && BLora.Channel() == 0); };
    }

    bool BadChannel       = false;
    bool LoseRocketFrames = false;
};

TEST_F(RadioChannelRetryTest, LostAckOfTheFirstRequestIsRetriedOnItsOwnAndBothEndsEndOnTheWantedChannel)
{
    AttachLora();
    Settle();
    ArrangeLostAckOnABadChannel();

    BadChannel       = true;
    LoseRocketFrames = true;
    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return BLora.Channel() == 2; }, 2000)) << "the rocket heard the request and moved";
    EXPECT_EQ(ALora.Channel(), 0) << "the ground never got the ack";
    LoseRocketFrames = false;

    // Both ends search and can only find each other on the old channel. The rocket reports that.
    ASSERT_TRUE(RunUntil([&] { return !B->Events.ChannelChanges.empty(); }, 30000));
    EXPECT_THAT(B->Events.ChannelChanges, ElementsAre(Pair(TransportKind::Lora, ChannelNumber {0})));
    EXPECT_THAT(A->Events.ChannelChanges, IsEmpty()) << "the ground does not report an attempt that failed";
    EXPECT_FALSE(A->R.RequestChannel(TransportKind::Lora, 1)) << "the request is still being worked on";

    // Channel 2 works from now on. Nobody calls RequestChannel again.
    BadChannel = false;
    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty(); }, 30000));
    ASSERT_TRUE(RunUntil([&] { return B->Events.ChannelChanges.size() == 2; }, 30000));
    Run(3000); // and it must stay that way, with no further reports

    EXPECT_EQ(ALora.Channel(), 2);
    EXPECT_EQ(BLora.Channel(), 2);
    EXPECT_THAT(A->Events.ChannelChanges, ElementsAre(Pair(TransportKind::Lora, ChannelNumber {2})))
        << "the ground reports once, with the wanted channel";
    EXPECT_THAT(B->Events.ChannelChanges,
                ElementsAre(Pair(TransportKind::Lora, ChannelNumber {0}), Pair(TransportKind::Lora, ChannelNumber {2})))
        << "the rocket saw the old channel on its way and then the wanted one";
    EXPECT_TRUE(A->R.IsLinkUp(TransportKind::Lora));
    EXPECT_TRUE(A->R.RequestChannel(TransportKind::Lora, 1)) << "the goal is done, a new request is accepted";

    ExpectCommandGetsThrough();
}

TEST_F(RadioChannelRetryTest, RequestThatIsAlwaysLostGivesUpAfterTheRetriesAndReportsWhereItLanded)
{
    AttachLora();
    Settle();
    ALora.Drop = [](std::span<const std::byte> Frame) { return IsSwitchRequest(Frame); };

    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty(); }, 120000));
    Run(5000); // nothing more is reported afterwards

    // one attempt and ConfigA.ChannelRequestRetries more, each of which ran out of attempts
    EXPECT_EQ(A->R.Stats().GivenUp, 1u + ConfigA.ChannelRequestRetries);
    EXPECT_THAT(A->Events.ChannelChanges, ElementsAre(Pair(TransportKind::Lora, ChannelNumber {0})));
    EXPECT_EQ(ALora.Channel(), 0);
    EXPECT_EQ(BLora.Channel(), 0);

    // and the goal is over: with a working link the same request now goes through
    ALora.Drop = nullptr;
    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return A->Events.ChannelChanges.size() == 2; }, 30000));
    EXPECT_THAT(A->Events.ChannelChanges.back(), Pair(TransportKind::Lora, ChannelNumber {2}));
    EXPECT_EQ(BLora.Channel(), 2);
}

TEST_F(RadioChannelRetryTest, ZeroRetriesIsTheOldSingleAttemptWhenTheRequestIsLost)
{
    ConfigA.ChannelRequestRetries = 0;
    SetUp();
    AttachLora();
    Settle();
    ALora.Drop = [](std::span<const std::byte> Frame) { return IsSwitchRequest(Frame); };

    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty(); }, 60000));
    Run(5000);

    EXPECT_EQ(A->R.Stats().GivenUp, 1u) << "one attempt, no more";
    EXPECT_THAT(A->Events.ChannelChanges, ElementsAre(Pair(TransportKind::Lora, ChannelNumber {0})));
    ALora.Drop = nullptr;
    EXPECT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2)) << "free again at once";
}

TEST_F(RadioChannelRetryTest, ZeroRetriesLeavesBothEndsOnTheOldChannelWhenTheAckIsLost)
{
    ConfigA.ChannelRequestRetries = 0;
    SetUp();
    AttachLora();
    Settle();
    ArrangeLostAckOnABadChannel();

    BadChannel       = true;
    LoseRocketFrames = true;
    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return BLora.Channel() == 2; }, 2000));
    LoseRocketFrames = false;
    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty() && !B->Events.ChannelChanges.empty(); }, 30000));

    // The app is told it ended up on the old channel, and nobody asks again
    BadChannel = false;
    Run(10000);
    EXPECT_THAT(A->Events.ChannelChanges, ElementsAre(Pair(TransportKind::Lora, ChannelNumber {0})));
    // The resent request makes the rocket try channel 2 once more together with the ground; each
    // time it lands it says where, and that is always the old channel
    EXPECT_THAT(B->Events.ChannelChanges, Each(Pair(TransportKind::Lora, ChannelNumber {0})));
    EXPECT_EQ(ALora.Channel(), 0);
    EXPECT_EQ(BLora.Channel(), 0);
}

TEST_F(RadioChannelRetryTest, LostAckNeedsNoRetryWhenTheResentRequestGetsThrough)
{
    ConfigA.ChannelRequestRetries = 0;
    SetUp();
    AttachLora();
    Settle();
    ArrangeLostAckOnABadChannel();

    // The rocket moves on the request, its ack is lost, and it comes back to the old channel.
    // The ground's resent request is then followed again, and its ack moves both ends together.
    LoseRocketFrames = true;
    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return BLora.Channel() == 2; }, 2000));
    LoseRocketFrames = false;

    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty(); }, 30000));
    Run(3000);
    EXPECT_THAT(A->Events.ChannelChanges, ElementsAre(Pair(TransportKind::Lora, ChannelNumber {2})));
    EXPECT_EQ(ALora.Channel(), 2);
    EXPECT_EQ(BLora.Channel(), 2);
    ExpectCommandGetsThrough();
}

TEST_F(RadioChannelRetryTest, RetryWaitsForRoomInTheReliableQueueAndRequestsAreRefusedMeanwhile)
{
    ConfigA.Sender.Capacity = 2;
    SetUp();
    AttachLora();
    Settle();

    // The first request is lost. Commands are lost too, so they stay pending (and fill the queue)
    // until they run out of attempts.
    bool LoseRequests = true;
    ALora.Drop        = [&](std::span<const std::byte> Frame)
    { return (LoseRequests && IsSwitchRequest(Frame)) || IsCommandFrame(Frame); };

    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return A->R.Stats().GivenUp == 1; }, 30000)) << "the first attempt is over";
    LoseRequests = false;
    ASSERT_EQ(A->R.Send(CommandMsg {CommandType::CameraOn}).Status, SendStatus::Queued);
    ASSERT_EQ(A->R.Send(CommandMsg {CommandType::CameraOff}).Status, SendStatus::Queued);

    // The ground finds the rocket on the old channel with no room to ask again
    Run(500);
    EXPECT_EQ(A->R.PendingReliable(), 2u) << "only the commands: the retry is waiting for a free place";
    EXPECT_FALSE(A->R.RequestChannel(TransportKind::Lora, 1)) << "a change is in progress";
    EXPECT_THAT(A->Events.ChannelChanges, IsEmpty());

    // Once the commands have been given up on, the retry goes in by itself and works
    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty(); }, 60000));
    Run(2000);
    EXPECT_EQ(A->R.Stats().GivenUp, 3u) << "the first request, then both commands, ran out of attempts; that made room";
    EXPECT_THAT(A->Events.FailedTickets, SizeIs(2));
    EXPECT_THAT(A->Events.ChannelChanges, ElementsAre(Pair(TransportKind::Lora, ChannelNumber {2})));
    EXPECT_EQ(ALora.Channel(), 2);
    EXPECT_EQ(BLora.Channel(), 2);
}

TEST_F(RadioChannelRetryTest, RequestIsRefusedWhileRetriesAreOutstandingAndOnlyThen)
{
    AttachLora();
    Settle();
    ALora.Drop = [](std::span<const std::byte> Frame) { return IsSwitchRequest(Frame); };

    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));

    // Refused on every poll until the final outcome has been reported
    while (A->Events.ChannelChanges.empty() && Now < 200000)
    {
        ASSERT_FALSE(A->R.RequestChannel(TransportKind::Lora, 1)) << "at " << Now << " ms";
        Run(5);
    }
    ASSERT_THAT(A->Events.ChannelChanges, SizeIs(1));
    ALora.Drop = nullptr;
    EXPECT_TRUE(A->R.RequestChannel(TransportKind::Lora, 1));
}

// The rocket hears the request and moves, its ack is lost, and the channel both were on goes dead.
// The ground loses the rocket, searches, and finds it on the wanted channel while its request is
// still unanswered. The answer then arrives with nothing left to do: that has to close the request.
TEST_F(RadioChannelRetryTest, RequestIsClosedWhenASearchReachedTheWantedChannelBeforeItsAck)
{
    AttachLora();
    Settle();

    // First to channel 2, so that going back to 0 is a move to a channel the search knows about
    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty() && !B->Events.ChannelChanges.empty(); }, 5000));
    ASSERT_EQ(ALora.Channel(), 2);
    ASSERT_EQ(BLora.Channel(), 2);
    Run(300);

    bool RocketMoved = false;
    ALora.Drop       = [&](std::span<const std::byte>) { return RocketMoved && ALora.Channel() == 2; };
    BLora.Drop       = [&](std::span<const std::byte>) { return BLora.Channel() == 2; };

    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 0));
    ASSERT_TRUE(RunUntil([&] { return BLora.Channel() == 0; }, 2000)) << "the rocket heard the request and moved";
    RocketMoved = true;

    ASSERT_TRUE(RunUntil([&] { return ALora.Channel() == 0 && A->R.IsLinkUp(TransportKind::Lora); }, 30000));
    EXPECT_EQ(A->R.Stats().GivenUp, 0u) << "found by searching, not because the request was given up on";

    ASSERT_TRUE(RunUntil([&] { return A->Events.ChannelChanges.size() == 2; }, 30000));
    Run(3000);

    EXPECT_THAT(A->Events.ChannelChanges,
                ElementsAre(Pair(TransportKind::Lora, ChannelNumber {2}), Pair(TransportKind::Lora, ChannelNumber {0})))
        << "the outcome is reported once";
    EXPECT_EQ(A->R.Stats().GivenUp, 0u);
    EXPECT_EQ(A->R.PendingReliable(), 0u);
    EXPECT_TRUE(OnSameChannel());

    ASSERT_NO_FATAL_FAILURE(ExpectCommandGetsThrough());
    EXPECT_TRUE(A->R.RequestChannel(TransportKind::Lora, 1)) << "the request is no longer open";
}

// The command before a channel request never arrives and the rocket's acks never get back, so the
// ground resends the request while the rocket still holds it behind the gap. It may only be acted
// on in order, once everything before it has arrived.
TEST_F(RadioChannelRetryTest, RequestHeldBehindAMissingMessageIsNotFollowedWhenItIsResent)
{
    AttachLora();
    Settle();
    ALora.Drop = [](std::span<const std::byte> Frame) { return IsCommandFrame(Frame); };
    BLora.Drop = [](std::span<const std::byte>) { return true; };

    ASSERT_EQ(A->R.Send(CommandMsg {CommandType::CameraOn}).Status, SendStatus::Queued); // always lost
    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));                              // held behind it

    // shorter than the command's give-up time: after that, the request would be next in order
    bool RocketLeft = false;
    for (int i = 0; i < 500; ++i)
    {
        Run(1);
        RocketLeft = RocketLeft || BLora.Channel() != 0;
    }
    EXPECT_THAT(B->Events.Commands(), IsEmpty());
    EXPECT_FALSE(RocketLeft);
}

// The ground moves 0 -> 2, then asks for 0 again. The rocket moves, its acks are lost, the ground
// finds it on 0 by searching, and then gives up on the request. There is nowhere else to look, so
// that is the outcome: it must be reported, and a new request accepted.
TEST_F(RadioChannelRetryTest, RequestIsClosedWhenGivenUpOnAfterASearchReachedTheWantedChannel)
{
    AttachLora();
    Settle();
    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(RunUntil([&] { return !A->Events.ChannelChanges.empty() && !B->Events.ChannelChanges.empty(); }, 5000));
    Run(300);

    bool RocketMoved = false;
    bool LoseAcks    = true;
    ALora.Drop = [&](std::span<const std::byte>) { return RocketMoved && ALora.Channel() == 2; };
    BLora.Drop = [&](std::span<const std::byte> Frame)
    { return BLora.Channel() == 2 || (LoseAcks && sim::CarriesAck(Frame)); };

    ASSERT_TRUE(A->R.RequestChannel(TransportKind::Lora, 0));
    ASSERT_TRUE(RunUntil([&] { return BLora.Channel() == 0; }, 2000));
    RocketMoved = true;
    ASSERT_TRUE(RunUntil([&] { return A->R.Stats().GivenUp == 1; }, 30000));
    ASSERT_EQ(ALora.Channel(), 0) << "the search had reached the wanted channel first";
    LoseAcks = false;

    ASSERT_TRUE(RunUntil([&] { return A->Events.ChannelChanges.size() == 2; }, 30000)) << "the request is never closed";
    EXPECT_THAT(A->Events.ChannelChanges,
                ElementsAre(Pair(TransportKind::Lora, ChannelNumber {2}), Pair(TransportKind::Lora, ChannelNumber {0})));
    EXPECT_TRUE(A->R.RequestChannel(TransportKind::Lora, 1)) << "the request is no longer open";
}
