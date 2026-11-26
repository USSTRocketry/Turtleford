// End to end tests of routing across LoRa and BLE, on two Radios joined by simulated transports
// (see Sim/RadioTest.h).

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <span>

#include "Mocks/MessageMatchers.h"
#include "Radio.h"
#include "Sim/Frames.h"
#include "Sim/RadioTest.h"

using namespace ra::turtleford;
using ra::type::CommandMsg;
using ra::type::CommandType;
using ra::type::DebugMsg;
using ra::type::StatusMsg;
using ra::type::FlightState;
using mocks::ArrivedReliably;
using mocks::ArrivedVia;
using mocks::IsCommand;
using mocks::IsKind;
using sim::IsReliableFrame;
using testing::AllOf;
using testing::AtLeast;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::Mock;
using testing::SizeIs;
using testing::UnorderedElementsAre;
using testing::_;
using sim::SampleFlightData;
using sim::LongText;
using sim::CommandAt;
using sim::RadioTest;

// ---------------------------------------------------------------------------
// Routing across transports
// ---------------------------------------------------------------------------

class RadioRoutingTest : public RadioTest
{
protected:
    void SettleBoth()
    {
        ASSERT_TRUE(RunUntil(
            [&]
            {
                return A->R.IsLinkUp(TransportKind::Lora) && B->R.IsLinkUp(TransportKind::Lora) &&
                       A->R.IsLinkUp(TransportKind::Ble) && B->R.IsLinkUp(TransportKind::Ble);
            },
            2000));
        Run(100);
    }

    // First, then Then: TransportKind::Any falls back to every other transport, None to nothing
    static Route Prefer(Delivery Mode, TransportKind First, TransportKind Then = TransportKind::Any)
    { return Route {.Mode = Mode, .Preference = {First, Then}}; }
};

TEST_F(RadioRoutingTest, DefaultRouteUsesTheFirstTransportAdded)
{
    AttachBoth();
    SettleBoth();

    B->R.Send(SampleFlightData());
    Run(50);

    EXPECT_THAT(A->Events.Messages, ElementsAre(AllOf(IsKind(MessageKind::FlightData), ArrivedVia(TransportKind::Lora))));
}

TEST_F(RadioRoutingTest, ConfiguredPreferenceIsUsed)
{
    ConfigB.Routes[static_cast<size_t>(MessageKind::FlightData)] = Prefer(Delivery::BestEffort, TransportKind::Ble);
    SetUp();
    AttachBoth();
    SettleBoth();

    B->R.Send(SampleFlightData());              // by its configured route
    B->R.Send(DebugMsg {.Status = 1, .Text = "x"}); // no preference configured
    Run(50);

    // (arrival order across two transports is not defined, so look each one up)
    EXPECT_THAT(A->Events.Messages,
                UnorderedElementsAre(AllOf(IsKind(MessageKind::FlightData), ArrivedVia(TransportKind::Ble)),
                                     AllOf(IsKind(MessageKind::Debug), ArrivedVia(TransportKind::Lora))));
}

TEST_F(RadioRoutingTest, PerSendRouteOverridesTheConfiguredOne)
{
    AttachBoth();
    SettleBoth();

    B->R.Send(SampleFlightData(), Prefer(Delivery::BestEffort, TransportKind::Ble));
    Run(50);

    EXPECT_THAT(A->Events.Messages, ElementsAre(AllOf(IsKind(MessageKind::FlightData), ArrivedVia(TransportKind::Ble))));
}

TEST_F(RadioRoutingTest, PerSendRouteCanMakeTelemetryReliable)
{
    AttachBoth();
    SettleBoth();

    const auto Result = B->R.Send(SampleFlightData(), Prefer(Delivery::Reliable, TransportKind::Ble));
    ASSERT_EQ(Result.Status, SendStatus::Queued);
    ASSERT_TRUE(RunUntil([&] { return !B->Events.DeliveredTickets.empty(); }, 2000));

    EXPECT_THAT(A->Events.Messages, ElementsAre(AllOf(IsKind(MessageKind::FlightData), ArrivedReliably())));
}

TEST_F(RadioRoutingTest, FallsBackWhenPreferredTransportIsNotReady)
{
    AttachBoth();
    SettleBoth();
    BBle.Ready = false; // e.g. BLE disconnected

    EXPECT_TRUE(B->R.Send(SampleFlightData(), Prefer(Delivery::BestEffort, TransportKind::Ble)));
    Run(50);

    EXPECT_THAT(A->Events.Messages, ElementsAre(AllOf(IsKind(MessageKind::FlightData), ArrivedVia(TransportKind::Lora))));
}

TEST_F(RadioRoutingTest, NoFallbackMeansNoRouteWhenPreferredIsNotReady)
{
    AttachBoth();
    SettleBoth();
    BBle.Ready = false;

    const auto Result = B->R.Send(SampleFlightData(), Prefer(Delivery::BestEffort, TransportKind::Ble, TransportKind::None));

    EXPECT_EQ(Result.Status, SendStatus::NoRoute);
    EXPECT_FALSE(Result);
}

// The list stops at the first None, so one that starts with None allows no transport at all
TEST_F(RadioRoutingTest, RouteThatListsNoTransportIsRefused)
{
    AttachBoth();
    SettleBoth();

    const Route Nothing {.Mode = Delivery::Reliable, .Preference = {}};
    const Route NoneFirst {.Mode = Delivery::Reliable, .Preference = {TransportKind::None, TransportKind::Lora}};

    EXPECT_EQ(A->R.Send(CommandMsg {CommandType::CameraOn}, Nothing).Status, SendStatus::NoRoute);
    EXPECT_EQ(A->R.Send(CommandMsg {CommandType::CameraOn}, NoneFirst).Status, SendStatus::NoRoute);
    EXPECT_EQ(A->R.PendingReliable(), 0u);
}

TEST_F(RadioRoutingTest, FallsBackWhenPeerIsUnreachableOnPreferredTransport)
{
    ObserveTransports();
    ABle.Cut = BBle.Cut = true; // BLE is powered but out of range
    AttachBoth();
    Settle();

    EXPECT_CALL(BBleTx, Send(_)).Times(0); // BLE is not tried at all
    B->R.Send(SampleFlightData(), Prefer(Delivery::BestEffort, TransportKind::Ble));
    Run(50);

    EXPECT_THAT(A->Events.Messages, ElementsAre(AllOf(IsKind(MessageKind::FlightData), ArrivedVia(TransportKind::Lora))));
}

TEST_F(RadioRoutingTest, WithoutFallbackItTransmitsBlindOnThePreferredTransport)
{
    ObserveTransports();
    ABle.Cut = BBle.Cut = true;
    AttachBoth();
    Settle();

    EXPECT_CALL(BBleTx, Send(_)).Times(1); // sent anyway, nobody hears it
    const auto Result = B->R.Send(SampleFlightData(), Prefer(Delivery::BestEffort, TransportKind::Ble, TransportKind::None));
    Run(50);

    EXPECT_EQ(Result.Status, SendStatus::Sent);
    EXPECT_THAT(A->Events.Messages, testing::Contains(IsKind(MessageKind::FlightData)).Times(0));
}

TEST_F(RadioRoutingTest, TransmitsBlindWhenNothingIsReachable)
{
    ObserveTransports();
    // nobody has heard anybody: the first frame still has to go out
    AttachBoth();

    EXPECT_CALL(BLoraTx, Send(_)).Times(1);
    EXPECT_EQ(B->R.Send(SampleFlightData()).Status, SendStatus::Sent);
    Run(1); // telemetry waits for the next Poll
}

TEST_F(RadioRoutingTest, RetransmissionMovesToAnotherTransportWhenOneDies)
{
    ObserveTransports();
    ConfigA.Sender.MaxAttempts = 12;
    SetUp();
    AttachBoth();
    SettleBoth();

    // BLE dies with a command on its way
    ABle.Cut = BBle.Cut = true;
    const auto Result       = A->R.Send(CommandMsg {CommandType::Abort}, Prefer(Delivery::Reliable, TransportKind::Ble));
    EXPECT_CALL(ALoraTx, Send(_)).Times(AtLeast(1)); // the command is retried here once BLE is given up on
    EXPECT_CALL(A->Listener, OnDelivered(Result.Id)).Times(1);
    EXPECT_CALL(A->Listener, OnFailed(_)).Times(0);
    Run(300);
    EXPECT_THAT(A->Events.DeliveredTickets, IsEmpty()) << "still trying BLE, which looks alive for now";

    ASSERT_TRUE(RunUntil([&] { return !A->Events.DeliveredTickets.empty(); }, 10000));

    EXPECT_FALSE(A->R.IsLinkUp(TransportKind::Ble));
    Mock::VerifyAndClearExpectations(&ALoraTx);
    EXPECT_THAT(B->Events.Messages, ElementsAre(AllOf(IsCommand(CommandType::Abort), ArrivedVia(TransportKind::Lora))))
        << "exactly once, although it travelled on two transports";
}

TEST_F(RadioRoutingTest, AckMayReturnOnADifferentTransportThanTheData)
{
    // B prefers LoRa for everything it sends, A sends the command over BLE
    AttachBoth();
    SettleBoth();
    ABle.Cut = false;
    BBle.Cut = true; // B can hear BLE but cannot answer on it

    EXPECT_CALL(A->Listener, OnDelivered(_)).Times(1);
    A->R.Send(CommandMsg {CommandType::CameraOn}, Prefer(Delivery::Reliable, TransportKind::Ble));
    // B's telemetry (on LoRa) carries the ack
    for (int i = 0; i < 10 && A->Events.DeliveredTickets.empty(); ++i)
    {
        B->R.Send(SampleFlightData());
        Run(20);
    }

    EXPECT_THAT(B->Events.Commands(), SizeIs(1));
}

TEST_F(RadioRoutingTest, MessageOnARouteWithNoAttachedTransportIsRefusedAtOnce)
{
    AttachLora(); // no BLE on either end
    Settle();
    const Route Reliable   = Prefer(Delivery::Reliable, TransportKind::Ble, TransportKind::None);
    Route Idle             = Prefer(Delivery::BestEffort, TransportKind::Ble, TransportKind::None);
    Idle.WhenIdle          = true;

    EXPECT_EQ(A->R.Send(CommandMsg {CommandType::CameraOn}, Reliable).Status, SendStatus::NoRoute);
    EXPECT_EQ(A->R.Send(DebugMsg {.Status = 1, .Text = LongText(MaxDebugTextLength * 2)}, Reliable).Status,
              SendStatus::NoRoute);
    EXPECT_EQ(A->R.Send(SampleFlightData(), Idle).Status, SendStatus::NoRoute);
    EXPECT_EQ(A->R.PendingReliable(), 0u);
}

TEST_F(RadioRoutingTest, MessageIsRefusedAtOnceWhileTheOnlyTransportItMayUseIsDown)
{
    AttachBoth();
    SettleBoth();
    ABle.Ready = false;
    const Route Reliable = Prefer(Delivery::Reliable, TransportKind::Ble, TransportKind::None);
    Route Idle           = Prefer(Delivery::BestEffort, TransportKind::Ble, TransportKind::None);
    Idle.WhenIdle        = true;

    EXPECT_EQ(A->R.Send(CommandMsg {CommandType::CameraOn}, Reliable).Status, SendStatus::NoRoute);
    EXPECT_EQ(A->R.Send(DebugMsg {.Status = 1, .Text = LongText(MaxDebugTextLength * 2)}, Reliable).Status,
              SendStatus::NoRoute);
    EXPECT_EQ(A->R.Send(SampleFlightData(), Idle).Status, SendStatus::NoRoute);
    EXPECT_EQ(A->R.PendingReliable(), 0u);

    // once it is back the same message is accepted and delivered over it
    ABle.Ready = true;
    const auto Result = A->R.Send(CommandMsg {CommandType::CameraOn}, Reliable);
    ASSERT_EQ(Result.Status, SendStatus::Queued);
    ASSERT_TRUE(RunUntil([&] { return !A->Events.DeliveredTickets.empty(); }, 5000));
    EXPECT_THAT(A->Events.DeliveredTickets, ElementsAre(Result.Id));
}

// Accepted while BLE was up, then BLE goes down for good before it gets out. It must be given up
// on, and must not hold up the command behind it, which may go over LoRa.
TEST_F(RadioRoutingTest, MessageWhoseOnlyTransportGoesDownIsGivenUpOnAndDoesNotHoldUpTheRest)
{
    AttachBoth();
    SettleBoth();

    const auto BleOnly = A->R.Send(CommandMsg {CommandType::CameraOn}, Prefer(Delivery::Reliable, TransportKind::Ble, TransportKind::None));
    ASSERT_EQ(BleOnly.Status, SendStatus::Queued);
    ABle.Ready              = false;
    const auto BleSentSince = ABle.Sent;
    const auto Normal       = A->R.Send(CommandMsg {CommandType::CameraOff});
    ASSERT_EQ(Normal.Status, SendStatus::Queued);

    ASSERT_TRUE(RunUntil([&] { return A->R.PendingReliable() == 0; }, 60000));
    EXPECT_THAT(A->Events.FailedTickets, ElementsAre(BleOnly.Id));
    EXPECT_THAT(A->Events.DeliveredTickets, ElementsAre(Normal.Id));
    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandType::CameraOff));
    EXPECT_EQ(ABle.Sent, BleSentSince) << "nothing went out on BLE while it was down";
}

// The same, but BLE is back within the message's attempts: both arrive, in the order sent
TEST_F(RadioRoutingTest, MessageWhoseOnlyTransportComesBackInTimeIsDeliveredInOrder)
{
    AttachBoth();
    SettleBoth();

    const auto BleOnly = A->R.Send(CommandMsg {CommandType::CameraOn}, Prefer(Delivery::Reliable, TransportKind::Ble, TransportKind::None));
    ABle.Ready = false;
    const auto Normal = A->R.Send(CommandMsg {CommandType::CameraOff});
    Run(2000);
    EXPECT_THAT(B->Events.Commands(), IsEmpty()) << "the one behind it waits, delivery is in order";

    ABle.Ready = true;
    ASSERT_TRUE(RunUntil([&] { return A->R.PendingReliable() == 0; }, 30000));
    EXPECT_THAT(A->Events.DeliveredTickets, ElementsAre(BleOnly.Id, Normal.Id));
    EXPECT_THAT(B->Events.Commands(), ElementsAre(CommandType::CameraOn, CommandType::CameraOff));
}

// A route that allows only BLE never puts the message on LoRa, retransmissions included
TEST_F(RadioRoutingTest, MessagesOnABleOnlyRouteNeverGoOverLoraEvenWhenRetransmitted)
{
    AttachBoth();
    SettleBoth();
    int ReliableOnLora = 0;
    ALora.Drop         = [&ReliableOnLora](std::span<const std::byte> Frame)
    {
        if (IsReliableFrame(Frame)) { ++ReliableOnLora; }
        return false;
    };
    ABle.LossPercent = 50;

    const Route BleOnly = Prefer(Delivery::Reliable, TransportKind::Ble, TransportKind::None);
    for (int i = 0; i < 10; ++i) { ASSERT_EQ(A->R.Send(CommandMsg {CommandAt(i)}, BleOnly).Status, SendStatus::Queued); }
    ASSERT_TRUE(RunUntil([&] { return A->R.PendingReliable() == 0; }, 60000));

    EXPECT_GT(A->R.Stats().Retransmissions, 0u) << "the loss must actually force retransmissions";
    EXPECT_EQ(ReliableOnLora, 0);
    EXPECT_THAT(A->Events.FailedTickets, IsEmpty());
    EXPECT_THAT(A->Events.DeliveredTickets, SizeIs(10));
}

TEST_F(RadioRoutingTest, HeldMessagesOfTwoKindsForTwoIdleTransportsGoOutOnTheSamePoll)
{
    AttachBoth();
    SettleBoth();
    const Route OnLora {.Mode = Delivery::BestEffort, .Preference = {TransportKind::Lora}, .WhenIdle = true};
    const Route OnBle {.Mode = Delivery::BestEffort, .Preference = {TransportKind::Ble}, .WhenIdle = true};
    const auto LoraBulk = BLora.BulkSent;
    const auto BleBulk  = BBle.BulkSent;

    // whichever kind is looked at first, the one after it must not be skipped
    ASSERT_TRUE(B->R.Send(SampleFlightData(), OnLora));
    ASSERT_TRUE(B->R.Send(StatusMsg {.State = FlightState::InFlight}, OnBle));
    Run(1);

    EXPECT_EQ(BLora.BulkSent, LoraBulk + 1);
    EXPECT_EQ(BBle.BulkSent, BleBulk + 1) << "both transports were idle, so neither kind has to wait";
}
