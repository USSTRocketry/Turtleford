#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "Mocks/FrameMatchers.h"
#include "Mocks/MockTelemetryRadio.h"

#include "Transport/Fragment.h"
#include "Transport/LoraTransport.h"

using namespace ra::turtleford;
using mocks::FrameIs;
using mocks::HasMessageId;
using mocks::HasSource;
using mocks::IsMarkedBulk;
using mocks::IsMarkedOver;
using mocks::MockTelemetryRadio;
using testing::AllOf;
using testing::DoDefault;
using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::Each;
using testing::FloatEq;
using testing::InSequence;
using testing::IsEmpty;
using testing::Mock;
using testing::NiceMock;
using testing::Not;
using testing::Return;
using testing::SizeIs;
using testing::_;

namespace
{
constexpr uint8_t Net = 0x42;

std::vector<std::byte> Pattern(size_t Size, uint8_t Start = 0)
{
    std::vector<std::byte> Out(Size);
    for (size_t i = 0; i < Size; ++i) { Out[i] = std::byte {static_cast<uint8_t>(Start + i)}; }
    return Out;
}

std::vector<std::byte> Copy(std::span<const std::byte> Data) { return {Data.begin(), Data.end()}; }

// Bytes Offset .. Offset + Length of a message: what one frame carries
std::vector<std::byte> Slice(const std::vector<std::byte>& Message, size_t Offset, size_t Length)
{
    return {Message.begin() + static_cast<std::ptrdiff_t>(Offset),
            Message.begin() + static_cast<std::ptrdiff_t>(Offset + Length)};
}

std::span<const std::byte> AsBytes(const std::vector<uint8_t>& Frame)
{
    return {reinterpret_cast<const std::byte*>(Frame.data()), Frame.size()};
}

// Small frames so a few dozen bytes already need several of them: 15 payload bytes per frame
LoraTransportConfig SmallConfig()
{
    LoraTransportConfig Out;
    Out.ChannelsMHz    = {903.0f, 915.0f, 927.0f};
    // Same address both ways, so a frame this transport sends is also one it accepts
    Out.Address        = Net;
    Out.PeerAddress    = Net;
    Out.FrameSize      = 20;
    Out.MaxMessageSize = 100;
    // These tests are about frames and messages, which must go out back to back; turn taking has its own below
    Out.ListenEveryMs = 0;
    return Out;
}

// Put the frames a radio sent back together into messages
std::vector<std::vector<std::byte>> Decode(const std::vector<std::vector<uint8_t>>& Frames)
{
    Reassembler R {Net, 512, 1000};
    std::vector<std::vector<std::byte>> Messages;
    for (const auto& Frame : Frames)
    {
        const auto Message = R.Push(AsBytes(Frame), 0);
        if (!Message.empty()) { Messages.push_back(Copy(Message)); }
    }
    EXPECT_EQ(R.Dropped(), 0u);
    return Messages;
}

// Frames as a peer with the given network id and frame size would put them on air
std::vector<std::vector<uint8_t>> FramesFor(uint8_t NetworkId,
                                            uint16_t FrameSize,
                                            std::span<const std::byte> Message,
                                            uint8_t Flags = 0)
{
    Fragmenter F {NetworkId, FrameSize};
    std::vector<std::vector<uint8_t>> Frames;
    EXPECT_TRUE(F.Begin(Message, Flags));
    while (!F.Done())
    {
        const auto Frame = F.Current();
        const auto* Data = reinterpret_cast<const uint8_t*>(Frame.data());
        Frames.emplace_back(Data, Data + Frame.size());
        F.Advance();
    }
    return Frames;
}

// Everything From has put on air arrives at To
void Ferry(sim::FakeTelemetryRadio& From, sim::FakeTelemetryRadio& To)
{
    for (auto& Frame : From.Sent) { To.RxQueue.push_back(std::move(Frame)); }
    From.Sent.clear();
}

class LoraTransportTest : public testing::Test
{
protected:
    // Build the transport. Calls the constructor makes on the radio (frequency, addresses) happen
    // here, so a test that expects them sets its EXPECT_CALLs first.
    void Start(const LoraTransportConfig& Config = SmallConfig()) { T.emplace(Radio, Config); }

    // Queue a message the radio will hand out later
    void Hear(std::vector<std::vector<uint8_t>> Frames)
    {
        for (auto& Frame : Frames) { Fake.RxQueue.push_back(std::move(Frame)); }
    }

    // Poll until everything accepted by Send() has gone out
    void Drain()
    {
        for (int i = 0; i < 1000 && !T->TxIdle(); ++i) { T->Poll(0); }
        ASSERT_TRUE(T->TxIdle());
    }

    // The radio's calls are not the point of most tests, so uninteresting ones are not reported.
    // Calls a test does set an expectation on are still checked strictly.
    NiceMock<MockTelemetryRadio> Radio;
    sim::FakeTelemetryRadio& Fake = Radio.Fake;
    std::optional<LoraTransport> T;
};
} // namespace

// ---- construction -----------------------------------------------------------

TEST_F(LoraTransportTest, ConstructorTunesToInitialChannel)
{
    auto Config           = SmallConfig();
    Config.InitialChannel = 2;

    EXPECT_CALL(Radio, setFrequency(FloatEq(927.0f))).Times(1);
    Start(Config);

    EXPECT_EQ(T->Channel(), 2);
}

TEST_F(LoraTransportTest, OutOfRangeInitialChannelFallsBackToChannelZero)
{
    auto Config           = SmallConfig();
    Config.InitialChannel = 3; // table has 0..2

    EXPECT_CALL(Radio, setFrequency(FloatEq(903.0f))).Times(1);
    Start(Config);

    EXPECT_EQ(T->Channel(), 0);
}

TEST_F(LoraTransportTest, CapsDescribeTheConfiguration)
{
    auto Config        = SmallConfig();
    Config.RoundTripMs = 650;

    Start(Config);

    EXPECT_THAT(T->Caps(),
                testing::AllOf(testing::Field("Kind", &TransportCaps::Kind, TransportKind::Lora),
                               testing::Field("ChannelCount", &TransportCaps::ChannelCount, 3),
                               testing::Field("MaxMessageSize", &TransportCaps::MaxMessageSize, 100),
                               testing::Field("RoundTripMs", &TransportCaps::RoundTripMs, 650u)));
    EXPECT_TRUE(T->IsReady());
}

// ---- sending ----------------------------------------------------------------

TEST_F(LoraTransportTest, SmallMessageGoesOutAsOneFrameOnTheNextPoll)
{
    Start();
    const auto Message = Pattern(10);

    // Send only queues, Poll talks to the radio
    EXPECT_CALL(Radio, send(_)).Times(0);
    ASSERT_TRUE(T->Send(Message));
    Mock::VerifyAndClearExpectations(&Radio);

    // exactly one frame, and nothing more however often it is polled afterwards
    EXPECT_CALL(Radio, send(testing::AllOf(SizeIs(FragmentHeader::Size + 10), FrameIs(Net, 0, 1, Message)))).Times(1);
    T->Poll(0);
    T->Poll(1);
    T->Poll(2);
}

TEST_F(LoraTransportTest, LargeMessageGoesOutOneFramePerPoll)
{
    Start();
    const auto Message = Pattern(40); // 15 + 15 + 10
    ASSERT_TRUE(T->Send(Message));

    // Each poll sends exactly one frame: the expectation is checked and cleared between polls
    const std::vector<std::pair<size_t, size_t>> Parts {{0, 15}, {15, 15}, {30, 10}};
    for (size_t i = 0; i < Parts.size(); ++i)
    {
        SCOPED_TRACE(testing::Message() << "poll " << i);
        const auto [Offset, Length] = Parts[i];
        EXPECT_CALL(Radio,
                    send(testing::AllOf(SizeIs(FragmentHeader::Size + Length), FrameIs(Net, i, 3, Slice(Message, Offset, Length)))))
            .Times(1);
        T->Poll(static_cast<uint32_t>(i));
        Mock::VerifyAndClearExpectations(&Radio);
    }

    EXPECT_CALL(Radio, send(_)).Times(0);
    T->Poll(3);
    Mock::VerifyAndClearExpectations(&Radio);

    ASSERT_THAT(Fake.Sent, SizeIs(3));
    EXPECT_THAT(Fake.Sent, Each(HasMessageId(Fake.Sent[0][1]))) << "same message id on every fragment";
}

TEST_F(LoraTransportTest, TxIdleOnlyOnceTheLastFrameIsOut)
{
    Start();

    EXPECT_TRUE(T->TxIdle());

    ASSERT_TRUE(T->Send(Pattern(40))); // 3 frames
    EXPECT_FALSE(T->TxIdle());

    T->Poll(0);
    EXPECT_FALSE(T->TxIdle());
    T->Poll(1);
    EXPECT_FALSE(T->TxIdle());
    T->Poll(2);
    EXPECT_TRUE(T->TxIdle());
}

TEST_F(LoraTransportTest, FailedSendRetriesTheSameFrame)
{
    Start();
    const auto Message = Pattern(40); // 3 frames
    const auto Frame0  = FrameIs(Net, 0, 3, Slice(Message, 0, 15));
    const auto Frame1  = FrameIs(Net, 1, 3, Slice(Message, 15, 15));
    const auto Frame2  = FrameIs(Net, 2, 3, Slice(Message, 30, 10));

    ASSERT_TRUE(T->Send(Message));

    {
        // The radio is busy from the very first frame and again in the middle of the message.
        // A refused frame comes back unchanged, the next one only after it has gone out.
        InSequence Order;
        EXPECT_CALL(Radio, send(Frame0)).Times(2).WillRepeatedly(Return(false));
        EXPECT_CALL(Radio, send(Frame0)).WillOnce(DoDefault());
        EXPECT_CALL(Radio, send(Frame1)).WillOnce(Return(false));
        EXPECT_CALL(Radio, send(Frame1)).WillOnce(DoDefault());
        EXPECT_CALL(Radio, send(Frame2)).WillOnce(DoDefault());

        T->Poll(0);
        T->Poll(1);
        EXPECT_FALSE(T->TxIdle());
        EXPECT_THAT(Fake.Sent, IsEmpty());

        T->Poll(2); // frame 0 at last
        T->Poll(3); // busy again
        ASSERT_NO_FATAL_FAILURE(Drain());
    }

    // on air: each frame exactly once, in order, and they make up the message
    EXPECT_THAT(Fake.Sent, ElementsAre(Frame0, Frame1, Frame2));
    EXPECT_THAT(Decode(Fake.Sent), ElementsAre(Message));
}

TEST_F(LoraTransportTest, MessagesGoOutInFifoOrder)
{
    Start();
    const auto First  = Pattern(5, 0x10);
    const auto Second = Pattern(40, 0x40); // chunked, in between two small ones
    const auto Third  = Pattern(7, 0x90);

    ASSERT_TRUE(T->Send(First));
    ASSERT_TRUE(T->Send(Second));
    ASSERT_TRUE(T->Send(Third));
    ASSERT_NO_FATAL_FAILURE(Drain());

    EXPECT_THAT(Fake.Sent, SizeIs(5));
    EXPECT_THAT(Decode(Fake.Sent), ElementsAre(First, Second, Third));
}

TEST_F(LoraTransportTest, SendRejectsEmptyAndOversizedMessages)
{
    Start(); // MaxMessageSize 100

    EXPECT_FALSE(T->Send({}));
    EXPECT_FALSE(T->Send(Pattern(101)));
    EXPECT_TRUE(T->TxIdle()) << "a rejected message must not be queued";

    EXPECT_CALL(Radio, send(_)).Times(0);
    T->Poll(0);
    Mock::VerifyAndClearExpectations(&Radio);

    // the limit itself is fine
    EXPECT_TRUE(T->Send(Pattern(100)));
}

TEST_F(LoraTransportTest, SendFailsWhenTheTxQueueIsFull)
{
    Start();

    std::vector<std::vector<std::byte>> Accepted;
    for (uint32_t i = 0; i < config::lora::TxQueueDepth; ++i)
    {
        Accepted.push_back(Pattern(5 + i, static_cast<uint8_t>(i)));
        ASSERT_TRUE(T->Send(Accepted.back())) << "message " << i;
    }
    EXPECT_FALSE(T->Send(Pattern(30, 0xA0)));

    ASSERT_NO_FATAL_FAILURE(Drain());

    // the rejected one left no trace
    EXPECT_THAT(Decode(Fake.Sent), ElementsAreArray(Accepted));

    // room again once the queue has emptied
    EXPECT_TRUE(T->Send(Pattern(30)));
}

// ---- receiving --------------------------------------------------------------

TEST_F(LoraTransportTest, ReceiveIsEmptyWhenTheRadioHasNothing)
{
    Start();

    EXPECT_THAT(T->Receive(0), IsEmpty());
    EXPECT_EQ(T->DroppedFrames(), 0u);
}

TEST_F(LoraTransportTest, ReceivesSingleFrameMessage)
{
    Start();
    const auto Message = Pattern(10, 0x30);

    Hear(FramesFor(Net, 20, Message));

    EXPECT_THAT(Copy(T->Receive(0)), ElementsAreArray(Message));
    EXPECT_THAT(T->Receive(1), IsEmpty());
    EXPECT_EQ(T->DroppedFrames(), 0u);
}

TEST_F(LoraTransportTest, ReassemblesMessageArrivingFrameByFrame)
{
    Start();
    const auto Message = Pattern(40, 0x30);
    const auto Frames  = FramesFor(Net, 20, Message);
    ASSERT_THAT(Frames, SizeIs(3));

    Fake.RxQueue.push_back(Frames[0]);
    EXPECT_THAT(T->Receive(0), IsEmpty());
    EXPECT_THAT(T->Receive(10), IsEmpty()) << "nothing new on air";

    Fake.RxQueue.push_back(Frames[1]);
    EXPECT_THAT(T->Receive(20), IsEmpty());

    Fake.RxQueue.push_back(Frames[2]);
    EXPECT_THAT(Copy(T->Receive(30)), ElementsAreArray(Message));
    EXPECT_EQ(T->DroppedFrames(), 0u);
}

TEST_F(LoraTransportTest, ReceiveHandsOutOneMessagePerCall)
{
    Start();
    const auto First  = Pattern(40, 0x30); // 3 frames, all already waiting in the radio
    const auto Second = Pattern(6, 0x70);

    // one fragmenter for both, as the sending end would, so the message ids differ
    Fragmenter F {Net, 20};
    for (const auto& Message : {First, Second})
    {
        ASSERT_TRUE(F.Begin(Message));
        while (!F.Done())
        {
            const auto Frame = F.Current();
            const auto* Data = reinterpret_cast<const uint8_t*>(Frame.data());
            Fake.RxQueue.emplace_back(Data, Data + Frame.size());
            F.Advance();
        }
    }

    EXPECT_THAT(Copy(T->Receive(0)), ElementsAreArray(First));
    EXPECT_THAT(Fake.RxQueue, SizeIs(1)) << "the second message stays in the radio until asked for";
    EXPECT_THAT(Copy(T->Receive(1)), ElementsAreArray(Second));
    EXPECT_THAT(T->Receive(2), IsEmpty());
}

TEST_F(LoraTransportTest, IgnoresFramesFromAnotherNetwork)
{
    Start();
    const auto Ours = Pattern(10, 0x30);

    Hear(FramesFor(0x99, 20, Pattern(10)));

    EXPECT_THAT(T->Receive(0), IsEmpty());
    EXPECT_EQ(T->DroppedFrames(), 1u);

    // a stranger in front of our own frame does not hide it
    Hear(FramesFor(0x99, 20, Pattern(10)));
    Hear(FramesFor(Net, 20, Ours));

    EXPECT_THAT(Copy(T->Receive(1)), ElementsAreArray(Ours));
    EXPECT_EQ(T->DroppedFrames(), 2u);
}

TEST_F(LoraTransportTest, TwoTransportsRoundTripLargeMessage)
{
    NiceMock<MockTelemetryRadio> RadioB;
    LoraTransportConfig Config; // defaults: 200 byte frames, so 400 bytes is 3 frames
    Config.ListenEveryMs = 0; // the three frames and the transport's idleness are counted poll by poll
    LoraTransportConfig ConfigB = Config; // the other end is the mirror image
    std::swap(ConfigB.Address, ConfigB.PeerAddress);
    LoraTransport A {Radio, Config};
    LoraTransport B {RadioB, ConfigB};

    const auto Message = Pattern(400, 0x11);
    ASSERT_TRUE(A.Send(Message));

    EXPECT_CALL(Radio, send(_)).Times(3);

    std::vector<std::byte> Received;
    int Polls = 0;
    for (uint32_t Now = 0; Now < 10 && !A.TxIdle(); ++Now)
    {
        A.Poll(Now);
        ++Polls;
        Ferry(Fake, RadioB.Fake);

        const auto Got = B.Receive(Now);
        if (!Got.empty())
        {
            EXPECT_TRUE(Received.empty()) << "delivered twice";
            Received = Copy(Got);
        }
    }

    EXPECT_TRUE(A.TxIdle());
    EXPECT_EQ(Polls, 3);
    EXPECT_THAT(Received, ElementsAreArray(Message));
    EXPECT_EQ(B.DroppedFrames(), 0u);
    Mock::VerifyAndClearExpectations(&Radio);

    // and the other way
    const auto Reply = Pattern(12, 0xC0);
    ASSERT_TRUE(B.Send(Reply));
    B.Poll(20);
    Ferry(RadioB.Fake, Fake);
    EXPECT_THAT(Copy(A.Receive(20)), ElementsAreArray(Reply));
    EXPECT_EQ(A.DroppedFrames(), 0u);
}

// ---- channels ---------------------------------------------------------------

TEST_F(LoraTransportTest, SetChannelRetunesTheRadio)
{
    // the constructor's tuning first, then one retune per SetChannel()
    InSequence Order;
    EXPECT_CALL(Radio, setFrequency(FloatEq(903.0f))).Times(1);
    EXPECT_CALL(Radio, setFrequency(FloatEq(927.0f))).Times(1);
    EXPECT_CALL(Radio, setFrequency(FloatEq(915.0f))).Times(1);

    Start();

    EXPECT_TRUE(T->SetChannel(2));
    EXPECT_EQ(T->Channel(), 2);

    EXPECT_TRUE(T->SetChannel(1));
    EXPECT_EQ(T->Channel(), 1);
}

TEST_F(LoraTransportTest, SetChannelRejectsIndexOutsideTheTable)
{
    auto Config           = SmallConfig();
    Config.InitialChannel = 1;

    // only the constructor tunes the radio
    EXPECT_CALL(Radio, setFrequency(FloatEq(915.0f))).Times(1);
    Start(Config);

    EXPECT_FALSE(T->SetChannel(3));
    EXPECT_FALSE(T->SetChannel(255));

    EXPECT_EQ(T->Channel(), 1);
}

TEST_F(LoraTransportTest, TxCompletedCountsMessagesOnceTheirLastFrameIsOut)
{
    Start(); // 15 payload bytes per frame

    EXPECT_EQ(T->TxCompleted(), 0u);
    ASSERT_TRUE(T->Send(Pattern(40))); // three frames
    ASSERT_TRUE(T->Send(Pattern(5)));  // one frame
    EXPECT_EQ(T->TxCompleted(), 0u) << "queued is not sent";

    // three frames go out, then the radio refuses one, then takes it
    InSequence Order;
    EXPECT_CALL(Radio, send(_)).Times(3);
    EXPECT_CALL(Radio, send(_)).WillOnce(Return(false));
    EXPECT_CALL(Radio, send(_)).WillOnce(DoDefault());

    T->Poll(0);
    T->Poll(1);
    EXPECT_EQ(T->TxCompleted(), 0u) << "two of three frames out";
    T->Poll(2);
    EXPECT_EQ(T->TxCompleted(), 1u);

    // a frame the radio refuses does not count
    T->Poll(3);
    EXPECT_EQ(T->TxCompleted(), 1u);

    T->Poll(4);
    EXPECT_EQ(T->TxCompleted(), 2u);
}

TEST_F(LoraTransportTest, RadioIsProgrammedWithOurAddressAndThePeers)
{
    LoraTransportConfig Config;
    Config.Address     = 0x21;
    Config.PeerAddress = 0x37;

    EXPECT_CALL(Radio, setAddress(0x21)).Times(1);
    EXPECT_CALL(Radio, setDestinationAddress(0x37)).Times(1);
    EXPECT_CALL(Radio, setPromiscuousMode(false)).Times(1); // the radio must filter on the destination address
    Start(Config);
}

TEST_F(LoraTransportTest, FramesCarryOurAddressAndOnlyThePeersAreAccepted)
{
    LoraTransportConfig Config;
    Config.Address     = 0x21;
    Config.PeerAddress = 0x37;
    Start(Config);

    ASSERT_TRUE(T->Send(Pattern(10)));
    EXPECT_CALL(Radio, send(HasSource(0x21))).Times(1); // sender address leads every frame
    T->Poll(0);
    Mock::VerifyAndClearExpectations(&Radio);

    // our own address coming back (an echo, or a device cloned with our settings) is not the peer
    const auto Wanted = Pattern(12, 0x60);
    Hear(FramesFor(0x21, 200, Pattern(12)));
    Hear(FramesFor(0x55, 200, Pattern(12)));
    Hear(FramesFor(0x37, 200, Wanted));

    EXPECT_THAT(Copy(T->Receive(0)), ElementsAreArray(Wanted));
    EXPECT_EQ(T->DroppedFrames(), 2u);
}

// ---- turn taking ------------------------------------------------------------

namespace
{
constexpr uint8_t Over = FragmentHeader::FlagOver;
constexpr uint8_t Bulk = FragmentHeader::FlagBulk;

// A message that is the last in the queue says "over" and is followed by a window of 100 to 125 ms;
// a bulk message does so once per 1000 ms. A peer that has not said "over" is waited for until it
// has been silent for 325 ms (a window of up to 125 and half a RoundTripMs of 400) or for 2000 ms. No guard time
// unless a test wants it.
class LoraTurnTakingTest : public LoraTransportTest
{
protected:
    static LoraTransportConfig TurnConfig()
    {
        auto Out          = SmallConfig();
        Out.ListenEveryMs = 1000;
        Out.ListenMs      = 100;
        Out.ListenGuardMs = 0;
        Out.RoundTripMs   = 400;
        return Out;
    }

    void StartTurnTaking() { Start(TurnConfig()); }

    // One small message into the queue
    void Queue(uint8_t First = 0, bool AsBulk = false) { ASSERT_TRUE(T->Send(Pattern(5, First), AsBulk)); }

    // The last frame of a peer message, carrying Flags, is in the radio when we look at NowMs
    void PeerFinishesAt(uint32_t NowMs, uint8_t Flags = 0)
    {
        Hear(FramesFor(Net, 20, Pattern(5, 0x70), Flags));
        EXPECT_FALSE(T->Receive(NowMs).empty());
    }
};

// How long each of Count listen windows of Transport lasts, measured by polling every millisecond
std::vector<uint32_t> WindowLengths(LoraTransport& Transport, int Count)
{
    std::vector<uint32_t> Lengths;
    uint32_t Now = 0;
    for (int i = 0; i < Count; ++i)
    {
        ++Now;
        EXPECT_TRUE(Transport.Send(Pattern(5))); // last in the queue: marked over
        Transport.Poll(Now);                     // goes out
        ++Now;
        Transport.Poll(Now); // the window starts here

        const uint32_t Start = Now;
        while (!Transport.TxIdle() && Now - Start < 1000)
        {
            ++Now;
            Transport.Poll(Now);
        }
        Lengths.push_back(Now - Start);
    }
    return Lengths;
}
} // namespace

// ---- saying "over" ----

TEST_F(LoraTurnTakingTest, NonBulkMessageLastInTheQueueIsMarkedOverAndFollowedByAWindow)
{
    StartTurnTaking();
    Queue(1);
    T->Poll(0);
    ASSERT_THAT(Fake.Sent, ElementsAre(AllOf(IsMarkedOver(), Not(IsMarkedBulk()))));
    EXPECT_FALSE(T->TxIdle());

    Queue(2);
    T->Poll(1); // the window starts at the first call after the send
    T->Poll(50);
    T->Poll(100);
    EXPECT_THAT(Fake.Sent, SizeIs(1)) << "the window is at least 100 ms";

    T->Poll(126); // and at most 125
    EXPECT_THAT(Fake.Sent, SizeIs(2));
}

TEST_F(LoraTurnTakingTest, NonBulkMessageWithAnotherQueuedBehindItIsNotMarked)
{
    StartTurnTaking();
    Queue(1);
    Queue(2);
    Queue(3);

    T->Poll(0);
    T->Poll(1);
    T->Poll(2);

    ASSERT_THAT(Fake.Sent, SizeIs(3));
    EXPECT_THAT(Fake.Sent, ElementsAre(Not(IsMarkedOver()), Not(IsMarkedOver()), IsMarkedOver()))
        << "only the last one has said what we had to say";
    EXPECT_FALSE(T->TxIdle());
}

TEST_F(LoraTurnTakingTest, BulkMessageIsMarkedBulkAndOverOnlyOncePerListenEveryMs)
{
    StartTurnTaking();

    uint32_t Now = 0;
    for (const uint32_t At : {0u, 200u, 400u, 999u, 1000u, 1500u, 1999u, 2000u})
    {
        Now = At;
        Queue(0, true);
        T->Poll(Now);
        T->Poll(Now + 1); // a window, if there is one, starts here
        T->Poll(Now + 140); // and is over
    }

    ASSERT_THAT(Fake.Sent, SizeIs(8));
    EXPECT_THAT(Fake.Sent, Each(IsMarkedBulk()));
    EXPECT_THAT(Fake.Sent,
                ElementsAre(IsMarkedOver(),
                            Not(IsMarkedOver()),
                            Not(IsMarkedOver()),
                            Not(IsMarkedOver()),
                            IsMarkedOver(),
                            Not(IsMarkedOver()),
                            Not(IsMarkedOver()),
                            IsMarkedOver()));
}

TEST_F(LoraTurnTakingTest, BulkMessagesNotMarkedOverAreFollowedByNoWindow)
{
    StartTurnTaking();
    Queue(1, true);
    T->Poll(0);
    T->Poll(1);
    T->Poll(140); // the first one was marked, its window is over

    Queue(2, true);
    Queue(3, true);
    T->Poll(141);
    T->Poll(142);

    EXPECT_THAT(Fake.Sent, SizeIs(3));
    EXPECT_TRUE(T->TxIdle());
}

TEST_F(LoraTurnTakingTest, NonBulkOverWeSendResetsThePeriodicSlotTimer)
{
    StartTurnTaking();
    Queue(1, true);
    T->Poll(0); // marked
    T->Poll(1);

    Queue(2); // not bulk, last in the queue: marked, 500 ms into the period
    T->Poll(500);
    T->Poll(501);

    Queue(3, true);
    T->Poll(1000); // 1000 ms after the first, but only 500 after the last over
    Queue(4, true);
    T->Poll(1499);
    Queue(5, true);
    T->Poll(1500); // 1000 ms after the second one

    ASSERT_THAT(Fake.Sent, SizeIs(5));
    EXPECT_THAT(Fake.Sent,
                ElementsAre(IsMarkedOver(), IsMarkedOver(), Not(IsMarkedOver()), Not(IsMarkedOver()), IsMarkedOver()));
}

TEST_F(LoraTurnTakingTest, OnlyTheLastFragmentOfAMessageCarriesTheMarksAndFragmentsGoOutOnConsecutivePolls)
{
    StartTurnTaking();
    ASSERT_TRUE(T->Send(Pattern(40, 0x10), true)); // three frames, bulk, the first of its kind: marked over
    Queue(2);

    T->Poll(0);
    T->Poll(1);
    T->Poll(2);
    ASSERT_THAT(Fake.Sent, SizeIs(3));
    EXPECT_THAT(Fake.Sent,
                ElementsAre(AllOf(Not(IsMarkedOver()), Not(IsMarkedBulk())),
                            AllOf(Not(IsMarkedOver()), Not(IsMarkedBulk())),
                            AllOf(IsMarkedOver(), IsMarkedBulk())));
    const std::vector<std::vector<uint8_t>> First {Fake.Sent.begin(), Fake.Sent.end()};

    T->Poll(3); // the window starts after the message, not inside it
    T->Poll(4);
    EXPECT_THAT(Fake.Sent, SizeIs(3));
    T->Poll(3 + 126);
    EXPECT_THAT(Fake.Sent, SizeIs(4));

    EXPECT_THAT(Decode(First), ElementsAre(Pattern(40, 0x10)));
}

TEST_F(LoraTurnTakingTest, TxIdleIsFalseDuringTheWindowAndTrueAfterIt)
{
    StartTurnTaking();
    Queue();
    T->Poll(0);
    T->Poll(1);
    EXPECT_FALSE(T->TxIdle());

    T->Poll(100);
    EXPECT_FALSE(T->TxIdle());
    T->Poll(126);
    EXPECT_TRUE(T->TxIdle());
}

TEST_F(LoraTurnTakingTest, WindowStartsAtTheFirstCallAfterTheSendNotAtTheSend)
{
    StartTurnTaking();
    Queue(1);
    T->Poll(0);
    Queue(2);

    T->Poll(1000); // the HAL blocked, or the application was busy: this is when we start listening
    T->Poll(1099);
    EXPECT_THAT(Fake.Sent, SizeIs(1));

    T->Poll(1126);
    EXPECT_THAT(Fake.Sent, SizeIs(2));
}

TEST_F(LoraTurnTakingTest, WindowSurvivesTheMillisecondClockWrapping)
{
    StartTurnTaking();
    Queue(1);

    const uint32_t Before = std::numeric_limits<uint32_t>::max() - 20;
    T->Poll(Before);
    Queue(2);
    T->Poll(Before + 5); // the window starts 16 ms before the clock wraps
    T->Poll(Before + 5 + 99);
    EXPECT_THAT(Fake.Sent, SizeIs(1)) << "the clock has wrapped, 100 ms have not passed";

    T->Poll(Before + 5 + 126);
    EXPECT_THAT(Fake.Sent, SizeIs(2));
}

TEST_F(LoraTurnTakingTest, PeriodicSlotIsOfferedAgainAfterListenEveryMsEvenWhenTheClockWrapsInBetween)
{
    StartTurnTaking();
    const uint32_t First = std::numeric_limits<uint32_t>::max() - 500;

    Queue(1, true);
    T->Poll(First);
    T->Poll(First + 1);
    Queue(2, true);
    T->Poll(First + 500); // wrapped, 500 ms later: too soon
    Queue(3, true);
    T->Poll(First + 999);
    Queue(4, true);
    T->Poll(First + 1000);

    EXPECT_THAT(Fake.Sent, ElementsAre(IsMarkedOver(), Not(IsMarkedOver()), Not(IsMarkedOver()), IsMarkedOver()));
}

TEST_F(LoraTurnTakingTest, WindowIsBetweenTheConfiguredTimeAndAQuarterMore)
{
    StartTurnTaking();

    const auto Lengths = WindowLengths(*T, 40);

    EXPECT_THAT(Lengths, Each(testing::AllOf(testing::Ge(100u), testing::Le(125u))));
    EXPECT_GT(*std::max_element(Lengths.begin(), Lengths.end()), *std::min_element(Lengths.begin(), Lengths.end()))
        << "the extra part is random";
}

TEST_F(LoraTurnTakingTest, TransportsWithDifferentAddressesDoNotPickTheSameWindows)
{
    StartTurnTaking();

    NiceMock<MockTelemetryRadio> RadioB;
    auto ConfigB    = TurnConfig();
    ConfigB.Address = static_cast<uint8_t>(Net + 1); // the other end of a link has its own address
    LoraTransport B {RadioB, ConfigB};

    EXPECT_NE(WindowLengths(*T, 20), WindowLengths(B, 20))
        << "two ends that stream would otherwise collide, wait the same time and collide again";
}

// ---- what a peer message ends do to our window ----

TEST_F(LoraTurnTakingTest, PeerOverEndsTheWindowAtOnce)
{
    StartTurnTaking();
    Queue(1);
    T->Poll(0);
    Queue(2);
    T->Poll(10);
    EXPECT_THAT(Fake.Sent, SizeIs(1));

    PeerFinishesAt(20, Over); // the peer has said what it had to say and is listening
    T->Poll(20);
    EXPECT_THAT(Fake.Sent, SizeIs(2));
}

TEST_F(LoraTurnTakingTest, PeerMessageEndWithoutOverDoesNotEndTheWindow)
{
    StartTurnTaking();
    Queue(1);
    T->Poll(0);
    Queue(2);
    T->Poll(10);

    PeerFinishesAt(20);
    T->Poll(20);

    EXPECT_THAT(Fake.Sent, SizeIs(1));
    EXPECT_FALSE(T->TxIdle());
}

TEST_F(LoraTurnTakingTest, PeerFragmentThatIsNotTheLastOneDoesNotEndTheWindow)
{
    StartTurnTaking();
    Queue(1);
    T->Poll(0);
    Queue(2);
    T->Poll(10);

    const auto Frames = FramesFor(Net, 20, Pattern(40, 0x30), Over); // three fragments, the last one marked
    ASSERT_THAT(Frames, SizeIs(3));

    Fake.RxQueue.push_back(Frames[0]);
    EXPECT_THAT(T->Receive(20), IsEmpty());
    Fake.RxQueue.push_back(Frames[1]);
    EXPECT_THAT(T->Receive(30), IsEmpty());
    T->Poll(30);
    EXPECT_THAT(Fake.Sent, SizeIs(1)) << "the peer is still talking";

    Fake.RxQueue.push_back(Frames[2]);
    EXPECT_THAT(Copy(T->Receive(40)), ElementsAreArray(Pattern(40, 0x30)));
    T->Poll(40);
    EXPECT_THAT(Fake.Sent, SizeIs(2));
}

TEST_F(LoraTurnTakingTest, LastFragmentOfAMarkedMessageWithMissingPartsStillEndsTheWindow)
{
    StartTurnTaking();
    Queue(1);
    T->Poll(0);
    Queue(2);
    T->Poll(10);

    // the first fragments were lost; the peer has finished talking all the same
    const auto Frames = FramesFor(Net, 20, Pattern(40, 0x30), Over);
    Fake.RxQueue.push_back(Frames[2]);
    EXPECT_THAT(T->Receive(20), IsEmpty());
    EXPECT_EQ(T->DroppedFrames(), 1u);

    T->Poll(20);
    EXPECT_THAT(Fake.Sent, SizeIs(2));
}

TEST_F(LoraTurnTakingTest, FrameFromAStrangerIsNotHeard)
{
    StartTurnTaking();
    Queue(1);
    T->Poll(0);
    Queue(2);
    T->Poll(10);

    Hear(FramesFor(0x99, 20, Pattern(5), Over)); // another device, however complete and marked its message
    EXPECT_THAT(T->Receive(20), IsEmpty());
    T->Poll(20);

    EXPECT_THAT(Fake.Sent, SizeIs(1));
    EXPECT_FALSE(T->TxIdle());
}

// ---- waiting for a peer that has more to say ----

TEST_F(LoraTurnTakingTest, NothingIsHeldBeforeThePeerHasSaidAnything)
{
    StartTurnTaking();
    Queue();

    T->Poll(0);

    EXPECT_THAT(Fake.Sent, SizeIs(1));
}

TEST_F(LoraTurnTakingTest, PeerMessageEndWithoutOverMakesUsHoldUntilItSaysOver)
{
    StartTurnTaking();
    PeerFinishesAt(0);
    Queue();

    T->Poll(0);
    T->Poll(50);
    EXPECT_THAT(Fake.Sent, IsEmpty());
    EXPECT_FALSE(T->TxIdle());

    PeerFinishesAt(100); // more of the same
    T->Poll(100);
    EXPECT_THAT(Fake.Sent, IsEmpty());

    PeerFinishesAt(150, Over);
    T->Poll(150);
    EXPECT_THAT(Fake.Sent, SizeIs(1)) << "on the poll right after its over";
}

TEST_F(LoraTurnTakingTest, AfterAPeerOverWeStayFreeToSend)
{
    StartTurnTaking();
    PeerFinishesAt(0);
    PeerFinishesAt(100, Over);
    Queue(1);
    T->Poll(100);
    ASSERT_THAT(Fake.Sent, SizeIs(1));
    T->Poll(101);
    Queue(2);
    T->Poll(400); // far later, with nothing more heard: the peer talks only when it has something to say

    EXPECT_THAT(Fake.Sent, SizeIs(2));
}

TEST_F(LoraTurnTakingTest, AfterAPeerBulkOverWeAreFreeForHalfAListenMsAndThenHoldAgain)
{
    StartTurnTaking();
    PeerFinishesAt(0, Bulk);
    PeerFinishesAt(100, Over | Bulk); // a slot in the middle of a stream
    Queue(1);

    T->Poll(149); // ListenMs / 2 = 50 ms after it
    EXPECT_THAT(Fake.Sent, SizeIs(1));
}

TEST_F(LoraTurnTakingTest, ABulkOverOnlyFreesUsForHalfAListenMs)
{
    StartTurnTaking();
    PeerFinishesAt(0, Bulk);
    PeerFinishesAt(100, Over | Bulk);
    Queue(1);

    T->Poll(150); // the stream is about to carry on
    EXPECT_THAT(Fake.Sent, IsEmpty());
    EXPECT_FALSE(T->TxIdle());

    PeerFinishesAt(160, Bulk);
    PeerFinishesAt(170, Over | Bulk); // and its next slot
    T->Poll(170);
    EXPECT_THAT(Fake.Sent, SizeIs(1));
}

TEST_F(LoraTurnTakingTest, OurAnswerWaitsForThePeersRadioToListenAgain)
{
    auto Config          = TurnConfig();
    Config.ListenGuardMs = 10; // the peer may not call receive() for up to this long after its frame
    Start(Config);
    PeerFinishesAt(0);
    Queue(1);

    PeerFinishesAt(20, Over);
    T->Poll(20);
    EXPECT_THAT(Fake.Sent, IsEmpty());
    EXPECT_FALSE(T->TxIdle());
    T->Poll(29);
    EXPECT_THAT(Fake.Sent, IsEmpty());

    T->Poll(30);
    EXPECT_THAT(Fake.Sent, SizeIs(1));
}

TEST_F(LoraTurnTakingTest, GuardWaitEndsAndTheFreeTimeOfABulkOverCountsFromTheOverNotFromTheGuard)
{
    auto Config          = TurnConfig();
    Config.ListenGuardMs = 10;
    Start(Config);
    PeerFinishesAt(0, Bulk);
    PeerFinishesAt(100, Over | Bulk);
    Queue(1);

    T->Poll(109);
    EXPECT_THAT(Fake.Sent, IsEmpty());
    T->Poll(110);
    EXPECT_THAT(Fake.Sent, SizeIs(1));
}

TEST_F(LoraTurnTakingTest, GuardWaitSurvivesTheMillisecondClockWrapping)
{
    auto Config          = TurnConfig();
    Config.ListenGuardMs = 10;
    Start(Config);
    const uint32_t At = std::numeric_limits<uint32_t>::max() - 4;
    PeerFinishesAt(At - 50);
    Queue();

    PeerFinishesAt(At, Over);
    T->Poll(At + 9);
    EXPECT_THAT(Fake.Sent, IsEmpty());
    T->Poll(At + 10); // wrapped
    EXPECT_THAT(Fake.Sent, SizeIs(1));
}

TEST_F(LoraTurnTakingTest, PeerThatGoesQuietWithoutSayingOverIsNoLongerWaitedForAfterAWindowAndHalfARoundTrip)
{
    StartTurnTaking();
    PeerFinishesAt(0);
    Queue();

    T->Poll(324);
    EXPECT_THAT(Fake.Sent, IsEmpty());
    T->Poll(325); // a window and half a round trip of silence
    EXPECT_THAT(Fake.Sent, SizeIs(1));
}

TEST_F(LoraTurnTakingTest, AnyFrameFromThePeerCountsAsNotQuietEvenIfNoMessageEnds)
{
    StartTurnTaking();
    PeerFinishesAt(0);
    Queue();

    const auto Frames = FramesFor(Net, 20, Pattern(40, 0x30)); // a message of three fragments, still coming in
    Fake.RxQueue.push_back(Frames[0]);
    EXPECT_THAT(T->Receive(150), IsEmpty());
    T->Poll(400); // 400 ms after the last message end, but 250 after a frame

    EXPECT_THAT(Fake.Sent, IsEmpty()) << "the peer was still sending 250 ms ago";
}

TEST_F(LoraTurnTakingTest, PeerThatNeverSaysOverIsGivenUpOnAfterTwiceListenEveryMs)
{
    StartTurnTaking();
    for (uint32_t At = 0; At <= 1900; At += 100) { PeerFinishesAt(At); } // never quiet, never over
    Queue();

    T->Poll(1999);
    EXPECT_THAT(Fake.Sent, IsEmpty());
    T->Poll(2000);
    EXPECT_THAT(Fake.Sent, SizeIs(1));
}

TEST_F(LoraTurnTakingTest, AfterGivingUpWeHoldAgainOnlyOnceThePeerHasSaidOver)
{
    StartTurnTaking();
    for (uint32_t At = 0; At <= 1900; At += 100) { PeerFinishesAt(At); }
    T->Poll(2000); // gives up
    EXPECT_TRUE(T->TxIdle());

    PeerFinishesAt(2050); // it still has not said over
    EXPECT_TRUE(T->TxIdle());

    PeerFinishesAt(2100, Over);
    PeerFinishesAt(2150); // now it is waited for again
    EXPECT_FALSE(T->TxIdle());
}

TEST_F(LoraTurnTakingTest, PeerOverResetsTheTimeWeHaveBeenHolding)
{
    StartTurnTaking();
    for (uint32_t At = 0; At <= 1500; At += 100) { PeerFinishesAt(At); }
    PeerFinishesAt(1550, Over | Bulk); // a slot in a stream
    for (uint32_t At = 1650; At <= 3500; At += 100) { PeerFinishesAt(At, Bulk); }

    T->Poll(3600);
    EXPECT_FALSE(T->TxIdle()) << "it has been 3600 ms since the first message, but not 2000 since its last over";
    T->Poll(3650);
    EXPECT_TRUE(T->TxIdle());
}

TEST_F(LoraTurnTakingTest, ZeroListenEveryNeverMarksNeverWaitsAndNeverHolds)
{
    auto Config          = TurnConfig();
    Config.ListenEveryMs = 0;
    Start(Config);
    Queue(1);
    Queue(2, true);
    Queue(3);

    T->Poll(0);
    T->Poll(0);
    T->Poll(0);

    ASSERT_THAT(Fake.Sent, SizeIs(3));
    EXPECT_THAT(Fake.Sent, Each(AllOf(Not(IsMarkedOver()), Not(IsMarkedBulk()))));
    EXPECT_TRUE(T->TxIdle());

    // a peer that never says over, and one that does, change nothing
    PeerFinishesAt(0);
    PeerFinishesAt(100);
    PeerFinishesAt(150, Over);
    PeerFinishesAt(200);
    Queue(4);
    T->Poll(250);
    EXPECT_THAT(Fake.Sent, SizeIs(4));
    EXPECT_TRUE(T->TxIdle());
}
