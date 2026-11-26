// End to end tests of reliable delivery, on two Radios joined by simulated transports (see
// Sim/RadioTest.h): acks, retransmission, ordering, holding behind a gap, giving up.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "Mocks/MessageMatchers.h"
#include "Radio.h"
#include "Sim/Frames.h"
#include "Sim/RadioTest.h"
#include "Sim/SimTransport.h"

using namespace ra::turtleford;
using ra::type::CommandMsg;
using ra::type::CommandType;
using ra::type::DebugMsg;
using ra::type::StatusMsg;
using ra::type::FlightData;
using ra::type::FlightState;
using mocks::IsCommand;
using mocks::IsFlightState;
using mocks::IsKind;
using mocks::MessageIsCommand;
using sim::SimTransport;
using testing::ElementsAreArray;
using testing::Field;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::SizeIs;
using testing::_;
using sim::DecodeFrame;
using sim::IsCommandFrame;
using sim::IsReliableFrame;
using sim::Node;
using sim::SampleFlightData;
using sim::LongText;
using sim::CommandAt;
using sim::Received;
using sim::RadioTest;

namespace
{
bool IsReliableCameraOn(std::span<const std::byte> Frame)
{
    const auto Decoded = DecodeFrame(Frame);
    return Decoded.has_value() && IsReliableFrame(*Decoded) && IsCommandFrame(*Decoded, CommandType::CameraOn);
}
} // namespace

// ---------------------------------------------------------------------------
// Reliable
// ---------------------------------------------------------------------------

TEST_F(RadioTest, CommandIsDeliveredAndAcknowledged)
{
    AttachLora();

    const auto Result = A->R.Send(CommandMsg {CommandType::CameraOn});
    ASSERT_EQ(Result.Status, SendStatus::Queued);
    ASSERT_NE(Result.Id, InvalidTicket);

    // acknowledged exactly once, even over the 500 ms that follow, and never reported failed
    EXPECT_CALL(A->Listener, OnDelivered(Result.Id)).Times(1);
    EXPECT_CALL(A->Listener, OnFailed(_)).Times(0);
    // and B's application is told once, as a reliable message
    EXPECT_CALL(B->Listener, OnMessage(MessageIsCommand(CommandType::CameraOn), Field(&RxInfo::Reliable, true)))
        .Times(1);

    // from a cold start: the two ends have never heard each other
    ASSERT_TRUE(RunUntil([&] { return !A->Events.DeliveredTickets.empty(); }, 2000));
    Run(500);

    EXPECT_EQ(A->R.PendingReliable(), 0u);
}

TEST_F(RadioTest, FlightStateChangeIsSentReliablyInControl)
{
    AttachLora();

    const auto Result = B->R.Send(StatusMsg {.State = FlightState::InFlight});
    ASSERT_EQ(Result.Status, SendStatus::Queued) << "state changes are reliable by default";
    ASSERT_TRUE(RunUntil([&] { return !B->Events.DeliveredTickets.empty(); }, 2000));

    EXPECT_THAT(A->Events.Messages, ElementsAre(IsFlightState(FlightState::InFlight)));
}

TEST_F(RadioTest, FlightControlWithoutStateRoundTrips)
{
    AttachLora();

    B->R.Send(StatusMsg {.State = std::nullopt});
    ASSERT_TRUE(RunUntil([&] { return !A->Events.Messages.empty(); }, 2000));

    EXPECT_THAT(A->Events.Messages, ElementsAre(IsFlightState(std::nullopt)));
}

TEST_F(RadioTest, BurstOfCommandsArrivesInOrder)
{
    AttachLora();
    Settle();

    std::vector<CommandType> Expected;
    for (int i = 0; i < 12; ++i)
    {
        Expected.push_back(CommandAt(i));
        ASSERT_TRUE(A->R.Send(CommandMsg {CommandAt(i)}));
    }

    ASSERT_TRUE(RunUntil([&] { return A->Events.DeliveredTickets.size() == 12; }, 5000));
    EXPECT_THAT(B->Events.Commands(), ElementsAreArray(Expected));
    EXPECT_EQ(A->R.Stats().Retransmissions, 0u) << "clean link";
}

TEST_F(RadioTest, DroppedFrameIsRetransmitted)
{
    AttachLora();
    Settle();

    // lose exactly the next thing A sends
    int ToDrop = 1;
    ALora.Drop = [&](std::span<const std::byte>) { return ToDrop-- > 0; };

    const auto Result = A->R.Send(CommandMsg {CommandType::StartRecording});
    EXPECT_CALL(A->Listener, OnDelivered(Result.Id)).Times(1);
    ASSERT_TRUE(RunUntil([&] { return !A->Events.DeliveredTickets.empty(); }, 2000));

    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandType::StartRecording));
    EXPECT_GE(A->R.Stats().Retransmissions, 1u);
}

TEST_F(RadioTest, LostAckDoesNotDeliverTwice)
{
    AttachLora();
    Settle();

    // B hears A, but nothing B says gets back
    BLora.Cut = true;
    A->R.Send(CommandMsg {CommandType::CameraOff});
    Run(450);

    EXPECT_THAT(B->Events.Commands(), SizeIs(1));
    EXPECT_THAT(A->Events.DeliveredTickets, IsEmpty());
    EXPECT_GE(A->R.Stats().Retransmissions, 1u) << "A kept trying, so B saw the command several times";

    BLora.Cut = false;
    ASSERT_TRUE(RunUntil([&] { return !A->Events.DeliveredTickets.empty(); }, 2000));
    Run(500);

    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandType::CameraOff));
}

namespace
{
// Lets through everything but the first frame that Matches
SimTransport::DropFilter DropFirst(std::function<bool(const Proto_MainMessage&)> Matches)
{
    return [Matches = std::move(Matches), Dropped = false](std::span<const std::byte> Frame) mutable
    {
        const auto Decoded = DecodeFrame(Frame);
        if (Dropped || !Decoded || !Matches(*Decoded)) { return false; }
        return Dropped = true;
    };
}

bool IsReliableCommand(const Proto_MainMessage& Msg)
{ return Msg.has_seq && Msg.which_message_type == Proto_MainMessage_command_tag; }
} // namespace

// The first of three commands is lost, so the other two wait behind it at the rocket. When it is
// resent, the rocket answers it from inside OnMessage, and that answer carries the ack. That ack
// has to cover the two released behind it, or the ground is left with two commands the rocket
// reported holding, which it never resends, and an ack that nothing makes the rocket send again.
class RadioReplyDuringReleaseTest : public RadioTest
{
protected:
    void ArrangeGapAndReply()
    {
        AttachLora();
        Settle();
        ALora.Drop = DropFirst(IsReliableCommand);

        ON_CALL(B->Listener, OnMessage(_, _))
            .WillByDefault(
                [this](const Message& Msg, const RxInfo& Info)
                {
                    B->Events.OnMessage(Msg, Info);
                    if (!Replied && std::holds_alternative<CommandMsg>(Msg))
                    {
                        Replied = true;
                        B->R.Send(DebugMsg {.Status = 1, .Text = "ok"}); // best effort: goes out at once
                    }
                });
    }

    bool Replied = false;
};

TEST_F(RadioReplyDuringReleaseTest, EveryCommandIsAcknowledgedWhenTheAnswerCarriesTheAck)
{
    ArrangeGapAndReply();

    for (int i = 0; i < 3; ++i) { A->R.Send(CommandMsg {CommandAt(i)}); }
    ASSERT_TRUE(RunUntil([&] { return A->Events.DeliveredTickets.size() == 3; }, 2000));

    EXPECT_TRUE(Replied);
    EXPECT_EQ(A->R.Stats().Retransmissions, 1u) << "only the lost one is sent again";
    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandAt(0), CommandAt(1), CommandAt(2)));
}

TEST_F(RadioReplyDuringReleaseTest, EveryCommandIsAcknowledgedWhenThatAnswerIsLostToo)
{
    ArrangeGapAndReply();
    BLora.Drop = DropFirst([](const Proto_MainMessage& Msg) { return Msg.has_ack && Msg.ack >= 3; });

    for (int i = 0; i < 3; ++i) { A->R.Send(CommandMsg {CommandAt(i)}); }
    ASSERT_TRUE(RunUntil([&] { return A->Events.DeliveredTickets.size() == 3; }, 10000));

    EXPECT_TRUE(Replied);
    EXPECT_THAT(A->Events.FailedTickets, IsEmpty());
    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandAt(0), CommandAt(1), CommandAt(2)));
}

// The rocket hears the ground, but nothing the rocket sends gets back. It runs a command and its
// own flight state runs out of attempts. Giving up on its own message must not make it forget what
// it has received: when the ground's command is sent again it is a repeat, and is not run twice.
TEST_F(RadioTest, CommandIsRunOnceWhenTheReceiverGivesUpOnAMessageOfItsOwn)
{
    ConfigA.Sender.MaxAttempts = 20; // the ground keeps trying for longer than the rocket does
    SetUp();
    AttachLora();
    Settle();

    BLora.Cut = true;
    B->R.Send(StatusMsg {.State = FlightState::InFlight});
    Run(500);
    A->R.Send(CommandMsg {CommandType::CameraOn});
    ASSERT_TRUE(RunUntil([&] { return B->Events.Commands().size() == 1; }, 3000));
    ASSERT_TRUE(RunUntil([&] { return !B->Events.FailedTickets.empty(); }, 60000)) << "the rocket gives up on its state";

    BLora.Cut = false;
    ASSERT_TRUE(RunUntil([&] { return !A->Events.DeliveredTickets.empty(); }, 30000));
    Run(3000);

    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandType::CameraOn)) << "run once";
    EXPECT_THAT(A->Events.FailedTickets, IsEmpty()) << "the ground's command did get through";
    EXPECT_THAT(B->Events.FailedTickets, SizeIs(1)) << "only the message that ran out of attempts";
}

// The first of two commands never gets through, the second does and waits behind it. When the
// ground gives up on the first, the rocket must be told to stop waiting even though the ground
// has nothing more to send and no keepalives run: otherwise the second is never delivered and the
// ground never hears that it was.
TEST_F(RadioTest, MessageWaitingBehindOneGivenUpOnIsDeliveredWhenNothingElseIsSent)
{
    ConfigA.KeepaliveMs = 0;
    ConfigB.KeepaliveMs = 0;
    SetUp();
    AttachLora();
    ALora.Drop = IsReliableCameraOn;

    const auto Lost = A->R.Send(CommandMsg {CommandType::CameraOn});
    const auto Kept = A->R.Send(CommandMsg {CommandType::CameraOff});
    ASSERT_TRUE(RunUntil([&] { return !A->Events.FailedTickets.empty() && !A->Events.DeliveredTickets.empty(); }, 20000));

    EXPECT_THAT(A->Events.FailedTickets, ElementsAre(Lost.Id));
    EXPECT_THAT(A->Events.DeliveredTickets, ElementsAre(Kept.Id));
    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandType::CameraOff));
    EXPECT_EQ(A->R.PendingReliable(), 0u);
}

TEST_F(RadioTest, DuplicatedFramesDeliverOnce)
{
    AttachLora();
    ALora.DupPercent = 100;
    BLora.DupPercent = 100;
    EXPECT_CALL(A->Listener, OnDelivered(_)).Times(5);

    for (int i = 0; i < 5; ++i) { A->R.Send(CommandMsg {CommandAt(i)}); }
    ASSERT_TRUE(RunUntil([&] { return A->Events.DeliveredTickets.size() == 5; }, 5000));
    Run(500);

    EXPECT_THAT(B->Events.Commands(), SizeIs(5));
}

TEST_F(RadioTest, AllCommandsSurviveALossyLinkInOrder)
{
    ConfigA.Sender.MaxAttempts = 40; // this test is about recovery, not about giving up
    SetUp();
    AttachLora();
    ALora.LossPercent = 30;
    BLora.LossPercent = 30;
    EXPECT_CALL(A->Listener, OnFailed(_)).Times(0);

    constexpr int Count = 60;
    std::vector<CommandType> Expected;
    int Submitted = 0;

    const auto Done = [&]
    {
        // feed commands in as room becomes available
        while (Submitted < Count && A->R.Send(CommandMsg {CommandAt(Submitted)}))
        {
            Expected.push_back(CommandAt(Submitted));
            ++Submitted;
        }
        return A->Events.DeliveredTickets.size() == static_cast<size_t>(Count);
    };

    ASSERT_TRUE(RunUntil(Done, 300000));
    EXPECT_THAT(B->Events.Commands(), ElementsAreArray(Expected));
    EXPECT_GT(A->R.Stats().Retransmissions, 0u);
}

TEST_F(RadioTest, BothDirectionsAtOnceOverALossyLink)
{
    ConfigA.Sender.MaxAttempts = 40;
    ConfigB.Sender.MaxAttempts = 40;
    SetUp();
    AttachLora();
    ALora.LossPercent = 25;
    BLora.LossPercent = 25;

    for (int i = 0; i < 10; ++i)
    {
        ASSERT_TRUE(A->R.Send(CommandMsg {CommandAt(i)}));
        ASSERT_TRUE(B->R.Send(StatusMsg {.State = static_cast<FlightState>(1 + i % 4)}));
    }

    ASSERT_TRUE(RunUntil(
        [&] { return A->Events.DeliveredTickets.size() == 10 && B->Events.DeliveredTickets.size() == 10; }, 120000));


    // in the order they were sent
    std::vector<testing::Matcher<const Received&>> Commands;
    std::vector<testing::Matcher<const Received&>> States;
    for (int i = 0; i < 10; ++i)
    {
        Commands.push_back(IsCommand(CommandAt(i)));
        States.push_back(IsFlightState(static_cast<FlightState>(1 + i % 4)));
    }
    EXPECT_THAT(B->Events.Messages, ElementsAreArray(Commands));
    EXPECT_THAT(A->Events.Messages, ElementsAreArray(States));
}

TEST_F(RadioTest, AckRidesOnOutgoingTelemetry)
{
    ObserveTransports();
    AttachLora();
    Settle();

    // B sends only its telemetry: no frame is spent on the ack alone
    EXPECT_CALL(BLoraTx, Send(_)).Times(20);
    EXPECT_CALL(A->Listener, OnDelivered(_)).Times(1);

    A->R.Send(CommandMsg {CommandType::SdLogOn});
    // the rocket streams telemetry faster than the ack delay
    for (int i = 0; i < 20; ++i)
    {
        ASSERT_TRUE(B->R.Send(SampleFlightData()));
        Run(20);
    }
}

TEST_F(RadioTest, AckGoesAloneWhenNothingElseIsSent)
{
    ObserveTransports();
    AttachLora();
    Settle();

    // one frame, the ack
    EXPECT_CALL(BLoraTx, Send(_)).Times(1);
    EXPECT_CALL(A->Listener, OnDelivered(_)).Times(1);

    A->R.Send(CommandMsg {CommandType::SdLogOff});
    Run(150);
}

// The owed ack must reach the peer: a keepalive on a transport where the peer is not heard (BLE,
// ready but out of range) must not take it
TEST_F(RadioTest, KeepaliveOnATransportThePeerIsNotHeardOnDoesNotTakeTheAck)
{
    AttachBoth();
    ABle.Cut = BBle.Cut = true;
    Settle();

    for (int i = 0; i < 20; ++i)
    {
        ASSERT_EQ(B->R.Send(CommandMsg {CommandAt(i)}).Status, SendStatus::Queued);
        Run(300);
    }
    ASSERT_THAT(B->Events.DeliveredTickets, SizeIs(20));
    EXPECT_EQ(B->R.Stats().Retransmissions, 0u) << "every ack reached the ground on LoRa";
}

TEST_F(RadioTest, ReliableQueueReportsFull)
{
    AttachLora();
    CutLora(true);

    for (uint16_t i = 0; i < ConfigA.Sender.Capacity; ++i)
    {
        ASSERT_EQ(A->R.Send(CommandMsg {CommandType::CameraOn}).Status, SendStatus::Queued);
    }

    EXPECT_EQ(A->R.Send(CommandMsg {CommandType::CameraOn}).Status, SendStatus::Full);
    EXPECT_EQ(A->R.PendingReliable(), ConfigA.Sender.Capacity);
}

TEST_F(RadioTest, MessageTooLargeForEveryTransportIsRejected)
{
    ALora.SetMaxMessageSize(40);
    AttachLora();

    EXPECT_EQ(A->R.Send(SampleFlightData()).Status, SendStatus::TooLarge);
    EXPECT_EQ(A->R.Send(CommandMsg {CommandType::Abort}).Status, SendStatus::Queued);
}

TEST_F(RadioTest, ReliableMessageTooLargeForEveryTransportItsRouteAllowsIsRefusedAtOnce)
{
    AttachBoth();
    ABle.SetMaxMessageSize(40); // FlightData fits LoRa but not BLE
    Settle();

    const Route BleOnly {.Mode = Delivery::Reliable, .Preference = {TransportKind::Ble}};
    EXPECT_EQ(A->R.Send(SampleFlightData(), BleOnly).Status, SendStatus::TooLarge);
    EXPECT_EQ(A->R.PendingReliable(), 0u);

    const auto Next = A->R.Send(CommandMsg {CommandType::CameraOn});
    ASSERT_TRUE(RunUntil([&] { return !A->Events.DeliveredTickets.empty(); }, 5000));
    EXPECT_THAT(A->Events.DeliveredTickets, ElementsAre(Next.Id)) << "nothing is left in front of it";
}

// ---------------------------------------------------------------------------
// Messages that wait in a transport's queue
// ---------------------------------------------------------------------------

TEST_F(RadioTest, SlowTransportQueueDoesNotCauseRetransmissions)
{
    AttachLora();
    Settle();

    // every message sits in the queue for six round trips before it goes on air
    ALora.QueueMs = 600;

    for (int i = 0; i < 3; ++i) { ASSERT_TRUE(A->R.Send(CommandMsg {CommandAt(i)})); }
    ASSERT_TRUE(RunUntil([&] { return A->Events.DeliveredTickets.size() == 3; }, 5000));

    EXPECT_EQ(A->R.Stats().Retransmissions, 0u) << "the timeout must not run while the message is still queued";
    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandAt(0), CommandAt(1), CommandAt(2)));
}

// A transport that sent messages before it was attached already counts them as completed. A message
// handed to it afterwards leaves when its count goes one higher, not when it reaches 0 + 1.
TEST_F(RadioTest, TransportThatCompletedMessagesBeforeItWasAttachedDoesNotStartTimersEarly)
{
    const std::byte Probe[] {std::byte {0}};
    ASSERT_TRUE(ALora.Send(Probe));
    ALora.Poll(Now);
    BLora.Receive(Now + ALora.DelayMs); // so that it is not heard by the radio later
    ASSERT_EQ(ALora.TxCompleted(), 1u);

    AttachLora();
    Settle();

    ALora.QueueMs = 2000; // well under the stall limit
    ASSERT_TRUE(A->R.Send(CommandMsg {CommandType::CameraOn}));
    Run(1500);

    EXPECT_EQ(A->R.Stats().Retransmissions, 0u) << "the timeout must not run while the message is still queued";
}

TEST_F(RadioTest, MessageStuckInATransportQueueIsSentAgainAfterTheStallLimit)
{
    ConfigA.TxStallMs = 1500;
    A                 = std::make_unique<Node>(ConfigA);
    AttachLora();
    Settle();

    ALora.QueueMs = 60000; // effectively never leaves
    ASSERT_TRUE(A->R.Send(CommandMsg {CommandType::CameraOn}));

    Run(1400);
    EXPECT_EQ(A->R.Stats().Retransmissions, 0u);
    Run(200);
    EXPECT_EQ(A->R.Stats().Retransmissions, 1u);
}

// ---------------------------------------------------------------------------
// Giving up, and coming back
// ---------------------------------------------------------------------------

TEST_F(RadioTest, CommandFailsWhenPeerIsGone)
{
    AttachLora();
    Settle();
    CutLora(true);

    const auto Result = A->R.Send(CommandMsg {CommandType::Abort});
    EXPECT_CALL(A->Listener, OnFailed(Result.Id)).Times(1);
    EXPECT_CALL(A->Listener, OnDelivered(_)).Times(0);
    ASSERT_TRUE(RunUntil([&] { return !A->Events.FailedTickets.empty(); }, 10000));

    EXPECT_EQ(A->R.Stats().GivenUp, 1u);
    EXPECT_EQ(A->R.PendingReliable(), 0u);
    EXPECT_THAT(B->Events.Commands(), IsEmpty());
}

TEST_F(RadioTest, EveryMessageToAPeerThatIsGoneFailsOnItsOwnInTheOrderSent)
{
    AttachLora();
    Settle();
    CutLora(true);

    std::vector<Ticket> Sent;
    for (int i = 0; i < 6; ++i) { Sent.push_back(A->R.Send(CommandMsg {CommandAt(i)}).Id); }
    ASSERT_TRUE(RunUntil([&] { return A->Events.FailedTickets.size() == 6; }, 30000));

    EXPECT_THAT(A->Events.FailedTickets, ElementsAreArray(Sent));
    EXPECT_EQ(A->R.Stats().GivenUp, 6u);
    EXPECT_EQ(A->R.PendingReliable(), 0u);
    EXPECT_THAT(A->Events.DeliveredTickets, IsEmpty());
}

TEST_F(RadioTest, LinkRecoversAfterFailure)
{
    AttachLora();
    Settle();
    CutLora(true);
    A->R.Send(CommandMsg {CommandType::CameraOn});
    ASSERT_TRUE(RunUntil([&] { return !A->Events.FailedTickets.empty(); }, 10000));

    CutLora(false);
    const auto Result = A->R.Send(CommandMsg {CommandType::CameraOff});
    EXPECT_CALL(A->Listener, OnDelivered(Result.Id)).Times(1);
    ASSERT_TRUE(RunUntil([&] { return !A->Events.DeliveredTickets.empty(); }, 5000));

    // the failed command was never heard, only the new one: the rocket stopped waiting for it
    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandType::CameraOff));

    // and the other direction still works after A gave up on a message
    B->R.Send(StatusMsg {.State = FlightState::MainChute});
    ASSERT_TRUE(RunUntil([&] { return !B->Events.DeliveredTickets.empty(); }, 5000));
    EXPECT_THAT(A->Events.Messages, testing::Contains(IsKind(MessageKind::Status)).Times(1));
}

TEST_F(RadioTest, CommandResentFromFailureCallbackIsDeliveredLater)
{
    AttachLora();
    Settle();
    CutLora(true);

    A->Events.OnFailedHook = [&](Ticket) { A->R.Send(CommandMsg {CommandType::Abort}); };
    A->R.Send(CommandMsg {CommandType::Abort});
    ASSERT_TRUE(RunUntil([&] { return !A->Events.FailedTickets.empty(); }, 10000));
    EXPECT_EQ(A->R.PendingReliable(), 1u);

    A->Events.OnFailedHook = nullptr;
    CutLora(false);
    ASSERT_TRUE(RunUntil([&] { return !A->Events.DeliveredTickets.empty(); }, 10000));

    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandType::Abort));
}

// 0 never arrives, 1 arrives and the rocket says it holds it, then the rocket is gone for good.
// Every message still needs an outcome; the one the rocket held must not block the rest.
TEST_F(RadioTest, MessageBehindOneThePeerHoldsStillFailsWhileThePeerIsGone)
{
    AttachLora();
    Settle();
    ALora.Drop = IsReliableCameraOn;

    const auto Lost = A->R.Send(CommandMsg {CommandType::CameraOn});
    const auto Held = A->R.Send(CommandMsg {CommandType::CameraOff});
    Run(200);
    CutLora(true);
    const auto Third = A->R.Send(CommandMsg {CommandType::StartRecording});

    ASSERT_TRUE(RunUntil([&] { return A->R.PendingReliable() == 0; }, 60000));
    EXPECT_THAT(A->Events.FailedTickets, ElementsAre(Lost.Id, Held.Id, Third.Id));
    EXPECT_THAT(A->Events.DeliveredTickets, IsEmpty());
}

// The same, but the rocket comes back: the held message is sent again with the news that the one
// before it was given up on, and is delivered and acknowledged
TEST_F(RadioTest, MessageThePeerHoldsIsDeliveredOnceTheOneBeforeItIsGivenUpOn)
{
    AttachLora();
    Settle();
    ALora.Drop = IsReliableCameraOn;

    const auto Lost = A->R.Send(CommandMsg {CommandType::CameraOn});
    const auto Held = A->R.Send(CommandMsg {CommandType::CameraOff});
    ASSERT_TRUE(RunUntil([&] { return A->R.PendingReliable() == 0; }, 30000));

    EXPECT_THAT(A->Events.FailedTickets, ElementsAre(Lost.Id));
    EXPECT_THAT(A->Events.DeliveredTickets, ElementsAre(Held.Id));
    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandType::CameraOff));
}

TEST_F(RadioTest, ReliableDebugTextWithAPartGivenUpOnIsReportedFailedOnceForTheWholeText)
{
    AttachLora();
    Settle();
    ALora.Drop = [](std::span<const std::byte> Frame)
    {
        const auto D = DecodeFrame(Frame);
        return D && D->has_seq && D->which_message_type == Proto_MainMessage_debug_tag && D->chunks_left == 1;
    };

    const auto Result =
        A->R.Send(DebugMsg {.Status = 1, .Text = LongText(MaxDebugTextLength * 3)}, Route {.Mode = Delivery::Reliable});
    ASSERT_EQ(Result.Status, SendStatus::Queued);
    ASSERT_TRUE(RunUntil([&] { return A->R.PendingReliable() == 0; }, 30000));

    EXPECT_THAT(A->Events.FailedTickets, ElementsAre(Result.Id)) << "one outcome, for the ticket the application has";
    EXPECT_THAT(A->Events.DeliveredTickets, IsEmpty());
    EXPECT_EQ(B->Events.Count(MessageKind::Debug), 2u) << "the parts after the missing one still arrive";
}
