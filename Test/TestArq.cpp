#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "Link/ArqReceiver.h"
#include "Link/ArqSender.h"

using namespace ra::turtleford;
using testing::AllOf;
using testing::ElementsAre;
using testing::Field;
using testing::InSequence;
using testing::IsEmpty;
using testing::MockFunction;
using testing::Mock;
using testing::Not;
using testing::NiceMock;
using testing::Optional;
using testing::Return;
using testing::StrictMock;
using testing::_;

namespace
{
using Sender   = ArqSender<int>;
using Receiver = ArqReceiver<int>;
using Result   = Receiver::Result;

constexpr uint32_t Rto = 100;

// Collect what is reported. Only for the runs of thousands of frames, where a mock call per
// frame would be slow and what is checked is the whole sequence at the end.
struct Sink
{
    std::vector<int> Items;
    auto Fn()
    {
        return [this](const int& Value) { Items.push_back(Value); };
    }
};

struct Tickets
{
    std::vector<Sender::Ticket> Items;
    auto Fn()
    {
        return [this](Sender::Ticket Id) { Items.push_back(Id); };
    }
};

// What a receiver hands to the application, and what a sender reports as delivered or failed.
// Strict: a call a test did not expect is a failure, so "nothing is delivered" needs no code.
using DeliveryMock = StrictMock<MockFunction<void(const int&)>>;
using TicketMock   = StrictMock<MockFunction<void(Sender::Ticket)>>;

// Send whatever is due and return the sequence numbers that went out
std::vector<SeqNum> Flush(Sender& Tx, uint32_t NowMs)
{
    std::vector<SeqNum> Sent;
    while (const auto Due = Tx.NextDue(NowMs))
    {
        Sent.push_back(Due->Seq);
        Tx.MarkSent(Due->Seq, NowMs, Rto);
    }
    return Sent;
}
} // namespace

// ---------------------------------------------------------------------------
// TimeReached
// ---------------------------------------------------------------------------

TEST(ArqTimeTest, HandlesClockWrap)
{
    EXPECT_TRUE(TimeReached(100, 100));
    EXPECT_TRUE(TimeReached(101, 100));
    EXPECT_FALSE(TimeReached(99, 100));

    // deadline just after the 32 bit wrap, now just before it
    EXPECT_FALSE(TimeReached(0xFFFFFFF0u, 0x00000010u));
    EXPECT_TRUE(TimeReached(0x00000010u, 0xFFFFFFF0u));
}

// ---------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------

TEST(ArqReceiverTest, DeliversInOrderFrames)
{
    Receiver Rx;
    DeliveryMock Out;
    {
        InSequence Order;
        EXPECT_CALL(Out, Call(10));
        EXPECT_CALL(Out, Call(11));
    }

    EXPECT_EQ(Rx.OnFrame(0, 10, Out.AsStdFunction()), Result::Delivered);
    EXPECT_EQ(Rx.OnFrame(1, 11, Out.AsStdFunction()), Result::Delivered);

    EXPECT_EQ(Rx.AckNext(), 2);
    EXPECT_EQ(Rx.AckSack(), 0);
}

TEST(ArqReceiverTest, HoldsOutOfOrderFramesUntilGapIsFilled)
{
    Receiver Rx;
    DeliveryMock Out;
    {
        // nothing is handed over before the gap is filled, then everything in order
        InSequence Order;
        EXPECT_CALL(Out, Call(10));
        EXPECT_CALL(Out, Call(11));
        EXPECT_CALL(Out, Call(12));
    }

    EXPECT_EQ(Rx.OnFrame(2, 12, Out.AsStdFunction()), Result::Buffered);
    EXPECT_EQ(Rx.OnFrame(1, 11, Out.AsStdFunction()), Result::Buffered);
    EXPECT_EQ(Rx.AckNext(), 0);
    EXPECT_EQ(Rx.AckSack(), 0b11); // seq 1 and 2 held

    EXPECT_EQ(Rx.OnFrame(0, 10, Out.AsStdFunction()), Result::Delivered);

    EXPECT_EQ(Rx.AckNext(), 3);
    EXPECT_EQ(Rx.AckSack(), 0);
}

TEST(ArqReceiverTest, ReleasesOnlyUpToTheNextGap)
{
    Receiver Rx;
    DeliveryMock Out;
    {
        // 13 is not among them
        InSequence Order;
        EXPECT_CALL(Out, Call(10));
        EXPECT_CALL(Out, Call(11));
    }

    Rx.OnFrame(1, 11, Out.AsStdFunction());
    Rx.OnFrame(3, 13, Out.AsStdFunction());
    Rx.OnFrame(0, 10, Out.AsStdFunction());

    EXPECT_EQ(Rx.AckNext(), 2);
    EXPECT_EQ(Rx.AckSack(), 0b1); // seq 3 = next + 1
}

TEST(ArqReceiverTest, DuplicatesAreNotDeliveredAgain)
{
    Receiver Rx;
    DeliveryMock Out;
    EXPECT_CALL(Out, Call(10)).Times(1); // the only delivery, whatever is repeated

    Rx.OnFrame(0, 10, Out.AsStdFunction());
    EXPECT_EQ(Rx.OnFrame(0, 10, Out.AsStdFunction()), Result::Duplicate); // already delivered

    Rx.OnFrame(2, 12, Out.AsStdFunction());
    EXPECT_EQ(Rx.OnFrame(2, 12, Out.AsStdFunction()), Result::Duplicate); // already buffered
}

TEST(ArqReceiverTest, DropsFramesBeyondTheWindow)
{
    Receiver Rx {{.Window = 4}};
    DeliveryMock Out; // nothing may be delivered

    EXPECT_EQ(Rx.OnFrame(3, 13, Out.AsStdFunction()), Result::Buffered);    // last slot of the window
    EXPECT_EQ(Rx.OnFrame(4, 14, Out.AsStdFunction()), Result::OutOfWindow); // one too far
    EXPECT_EQ(Rx.AckSack(), 0b100);
}

TEST(ArqReceiverTest, EveryFrameRequestsAnAck)
{
    Receiver Rx {{.Window = 4}};
    NiceMock<MockFunction<void(const int&)>> Out; // what is delivered is not the point here
    EXPECT_FALSE(Rx.AckPending());

    Rx.OnFrame(0, 10, Out.AsStdFunction());
    EXPECT_TRUE(Rx.AckPending());
    Rx.AckSent();
    EXPECT_FALSE(Rx.AckPending());

    // a duplicate means our ack was lost: it must be sent again
    Rx.OnFrame(0, 10, Out.AsStdFunction());
    EXPECT_TRUE(Rx.AckPending());
    Rx.AckSent();

    Rx.OnFrame(100, 1, Out.AsStdFunction()); // out of window still gets told where we are
    EXPECT_TRUE(Rx.AckPending());
}

TEST(ArqReceiverTest, AckBuiltWhileDeliveringAlreadyCoversTheFramesReleasedBehindIt)
{
    Receiver Rx {{.Window = 4}};
    NiceMock<MockFunction<void(const int&)>> Ignore;
    Rx.OnFrame(1, 11, Ignore.AsStdFunction());
    Rx.OnFrame(2, 12, Ignore.AsStdFunction());
    Rx.AckSent();

    // The application answers the first message, and that answer carries the ack, before 1 and 2
    // reach it. The ack must say all three are in, with nothing held.
    MockFunction<void(const int&)> Out;
    {
        InSequence Order;
        EXPECT_CALL(Out, Call(10))
            .WillOnce(
                [&Rx](const int&)
                {
                    EXPECT_EQ(Rx.AckNext(), 3);
                    EXPECT_EQ(Rx.AckSack(), 0);
                    Rx.AckSent();
                });
        EXPECT_CALL(Out, Call(11));
        EXPECT_CALL(Out, Call(12));
    }
    Rx.OnFrame(0, 10, Out.AsStdFunction());

    EXPECT_FALSE(Rx.AckPending()) << "that answer was the whole ack, no second one is owed";
}

TEST(ArqReceiverTest, SkipReleasesWhatWasHeldBehindTheMessageGivenUpOn)
{
    Receiver Rx {{.Window = 4}};
    NiceMock<MockFunction<void(const int&)>> Ignore;
    Rx.OnFrame(1, 11, Ignore.AsStdFunction());
    Rx.OnFrame(2, 12, Ignore.AsStdFunction());
    Rx.AckSent();

    // the sender gave up on 0
    MockFunction<void(const int&)> Out;
    {
        InSequence Order;
        EXPECT_CALL(Out, Call(11)).WillOnce([&Rx](const int&) { EXPECT_EQ(Rx.AckNext(), 3) << "ack already complete"; });
        EXPECT_CALL(Out, Call(12));
    }
    Rx.OnSkip(1, Out.AsStdFunction());

    EXPECT_EQ(Rx.AckNext(), 3);
    EXPECT_EQ(Rx.AckSack(), 0);
    EXPECT_TRUE(Rx.AckPending()) << "the sender keeps telling us until our ack shows we moved on";
}

TEST(ArqReceiverTest, SkipAlsoDeliversAFrameThatArrivedBeforeTheNewOldest)
{
    // The sender gave up on 0, 1 and 2, but 2 did arrive (its ack was lost). It is delivered: the
    // sender reported it failed, which means it may or may not have arrived.
    Receiver Rx {{.Window = 4}};
    NiceMock<MockFunction<void(const int&)>> Ignore;
    Rx.OnFrame(2, 12, Ignore.AsStdFunction());

    MockFunction<void(const int&)> Out;
    EXPECT_CALL(Out, Call(12));
    Rx.OnSkip(3, Out.AsStdFunction());
    EXPECT_EQ(Rx.AckNext(), 3);
}

TEST(ArqReceiverTest, SkipFarBeyondTheWindowJumpsThere)
{
    Receiver Rx {{.Window = 4}};
    NiceMock<MockFunction<void(const int&)>> Ignore;
    Rx.OnFrame(2, 12, Ignore.AsStdFunction());

    MockFunction<void(const int&)> Out;
    {
        InSequence Order;
        EXPECT_CALL(Out, Call(12)); // held before the new oldest
        EXPECT_CALL(Out, Call(1000));
    }
    Rx.OnSkip(1000, Out.AsStdFunction()); // a long outage: everything up to 999 was given up on
    EXPECT_EQ(Rx.AckNext(), 1000);
    EXPECT_EQ(Rx.AckSack(), 0) << "nothing from before the jump is left in the window";

    EXPECT_EQ(Rx.OnFrame(1000, 1000, Out.AsStdFunction()), Result::Delivered);
}

TEST(ArqReceiverTest, SkipThatIsOldOrAlreadyDoneChangesNothingButIsAnswered)
{
    Receiver Rx {{.Window = 4}};
    NiceMock<MockFunction<void(const int&)>> Ignore;
    for (SeqNum Seq = 0; Seq < 5; ++Seq) { Rx.OnFrame(Seq, Seq, Ignore.AsStdFunction()); }
    Rx.AckSent();

    DeliveryMock Out; // strict: nothing may be delivered
    Rx.OnSkip(5, Out.AsStdFunction());
    Rx.OnSkip(3, Out.AsStdFunction());

    EXPECT_EQ(Rx.AckNext(), 5);
    EXPECT_TRUE(Rx.AckPending());
}

TEST(ArqReceiverTest, ReleasingAFullWindowDeliversEveryHeldFrameOnce)
{
    Receiver Rx {{.Window = 4}};
    NiceMock<MockFunction<void(const int&)>> Ignore;
    for (SeqNum Seq = 1; Seq < 4; ++Seq) { Rx.OnFrame(Seq, 10 + Seq, Ignore.AsStdFunction()); }

    MockFunction<void(const int&)> Out;
    {
        InSequence Order;
        for (int Value = 10; Value < 14; ++Value) { EXPECT_CALL(Out, Call(Value)); }
    }
    EXPECT_EQ(Rx.OnFrame(0, 10, Out.AsStdFunction()), Result::Delivered);
    EXPECT_EQ(Rx.AckNext(), 4);
    EXPECT_EQ(Rx.AckSack(), 0);

    // the slots are free again: the next window's frames are held, not mistaken for old ones
    EXPECT_EQ(Rx.OnFrame(5, 15, Ignore.AsStdFunction()), Result::Buffered);
    EXPECT_EQ(Rx.AckSack(), 0b1);
}

TEST(ArqReceiverTest, WindowOfOneIsStopAndWait)
{
    Receiver Rx {{.Window = 1}};
    DeliveryMock Out;
    {
        InSequence Order;
        EXPECT_CALL(Out, Call(10));
        EXPECT_CALL(Out, Call(11));
    }

    EXPECT_EQ(Rx.OnFrame(1, 11, Out.AsStdFunction()), Result::OutOfWindow);
    EXPECT_EQ(Rx.OnFrame(0, 10, Out.AsStdFunction()), Result::Delivered);
    EXPECT_EQ(Rx.OnFrame(1, 11, Out.AsStdFunction()), Result::Delivered);
    EXPECT_EQ(Rx.AckSack(), 0);
}

TEST(ArqReceiverTest, ResetStartsAgainFromZero)
{
    Receiver Rx;
    DeliveryMock Out;
    {
        // 12 was buffered before the reset and must never come out
        InSequence Order;
        EXPECT_CALL(Out, Call(10));
        EXPECT_CALL(Out, Call(20));
    }
    Rx.OnFrame(0, 10, Out.AsStdFunction());
    Rx.OnFrame(2, 12, Out.AsStdFunction());

    Rx.Reset();

    EXPECT_EQ(Rx.AckNext(), 0);
    EXPECT_EQ(Rx.AckSack(), 0);
    EXPECT_FALSE(Rx.AckPending());
    EXPECT_EQ(Rx.OnFrame(0, 20, Out.AsStdFunction()), Result::Delivered);
}

TEST(ArqReceiverTest, DeliversAcrossSequenceWrap)
{
    Receiver Rx {{.Window = 4}};
    Sink Out;

    for (uint32_t i = 0; i < 70000; ++i) { Rx.OnFrame(static_cast<SeqNum>(i), static_cast<int>(i), Out.Fn()); }

    ASSERT_EQ(Out.Items.size(), 70000u);
    EXPECT_EQ(Out.Items.back(), 69999);
    EXPECT_EQ(Rx.AckNext(), static_cast<SeqNum>(70000));
}

TEST(ArqReceiverTest, ReordersAcrossSequenceWrap)
{
    Receiver Rx {{.Window = 4}};
    Sink Out;
    for (uint32_t i = 0; i < 65535; ++i) { Rx.OnFrame(static_cast<SeqNum>(i), 0, Out.Fn()); }

    // expected is now 65535; 0 and 1 are the two after it
    DeliveryMock Delivered;
    {
        InSequence Order;
        EXPECT_CALL(Delivered, Call(1));
        EXPECT_CALL(Delivered, Call(2));
        EXPECT_CALL(Delivered, Call(3));
    }
    EXPECT_EQ(Rx.OnFrame(1, 3, Delivered.AsStdFunction()), Result::Buffered);
    EXPECT_EQ(Rx.OnFrame(0, 2, Delivered.AsStdFunction()), Result::Buffered);
    EXPECT_EQ(Rx.OnFrame(65535, 1, Delivered.AsStdFunction()), Result::Delivered);

    EXPECT_EQ(Rx.AckNext(), 2);
}

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------

TEST(ArqSenderTest, SubmitGivesDistinctValidTickets)
{
    Sender Tx;

    const auto A = Tx.Submit(1);
    const auto B = Tx.Submit(2);

    EXPECT_NE(A, Sender::InvalidTicket);
    EXPECT_NE(B, Sender::InvalidTicket);
    EXPECT_NE(A, B);
    EXPECT_EQ(Tx.Pending(), 2u);
}

TEST(ArqSenderTest, RejectsWhenFull)
{
    Sender Tx {{.Window = 2, .Capacity = 3}};

    EXPECT_NE(Tx.Submit(1), Sender::InvalidTicket);
    EXPECT_NE(Tx.Submit(2), Sender::InvalidTicket);
    EXPECT_NE(Tx.Submit(3), Sender::InvalidTicket);
    EXPECT_TRUE(Tx.Full());
    EXPECT_EQ(Tx.Submit(4), Sender::InvalidTicket);
}

TEST(ArqSenderTest, SendsOnlyWhatFitsInTheWindow)
{
    Sender Tx {{.Window = 2, .Capacity = 8}};
    for (int i = 0; i < 5; ++i) { Tx.Submit(i); }

    EXPECT_THAT(Flush(Tx, 0), ElementsAre(0, 1));
    EXPECT_THAT(Tx.NextDue(0), Not(Optional(_)));
}

TEST(ArqSenderTest, FrameCarriesPayloadTicketAndAttempt)
{
    Sender Tx;
    const auto Id = Tx.Submit(42);

    const auto First = Tx.NextDue(0);
    EXPECT_THAT(First,
                Optional(AllOf(Field("Seq", &Sender::Frame::Seq, 0),
                               Field("Id", &Sender::Frame::Id, Id),
                               Field("Attempt", &Sender::Frame::Attempt, 0),
                               Field("Payload", &Sender::Frame::Payload, testing::Pointee(42)))));

    Tx.MarkSent(0, 0, Rto);
    EXPECT_THAT(Tx.NextDue(Rto), Optional(Field("Attempt", &Sender::Frame::Attempt, 1)));
}

TEST(ArqSenderTest, CumulativeAckSlidesTheWindowAndReportsDelivery)
{
    Sender Tx {{.Window = 2, .Capacity = 8}};
    TicketMock Done;
    const auto A = Tx.Submit(1);
    const auto B = Tx.Submit(2);
    Tx.Submit(3);
    Flush(Tx, 0);

    {
        InSequence Order;
        EXPECT_CALL(Done, Call(A));
        EXPECT_CALL(Done, Call(B));
    }
    Tx.OnAck(2, 0, 10, Done.AsStdFunction());

    EXPECT_EQ(Tx.Pending(), 1u);
    EXPECT_THAT(Flush(Tx, 10), ElementsAre(2)); // the third one moved into the window
}

TEST(ArqSenderTest, RetransmitsAfterTimeoutWithBackoff)
{
    Sender Tx {{.Window = 1, .Capacity = 4, .MaxAttempts = 6, .MaxBackoffShift = 2}};
    Tx.Submit(1);

    EXPECT_THAT(Flush(Tx, 0), testing::SizeIs(1));
    EXPECT_THAT(Tx.NextDue(99), Not(Optional(_)));
    EXPECT_THAT(Flush(Tx, 100), testing::SizeIs(1)); // after 1x

    EXPECT_THAT(Tx.NextDue(299), Not(Optional(_)));
    EXPECT_THAT(Flush(Tx, 300), testing::SizeIs(1)); // after 2x

    EXPECT_THAT(Tx.NextDue(699), Not(Optional(_)));
    EXPECT_THAT(Flush(Tx, 700), testing::SizeIs(1)); // after 4x

    EXPECT_THAT(Tx.NextDue(1099), Not(Optional(_)));
    EXPECT_THAT(Flush(Tx, 1100), testing::SizeIs(1)); // capped at 4x
}

TEST(ArqSenderTest, AckedMessageIsNotRetransmitted)
{
    Sender Tx;
    NiceMock<MockFunction<void(Sender::Ticket)>> Done;
    Tx.Submit(1);
    Flush(Tx, 0);

    Tx.OnAck(1, 0, 10, Done.AsStdFunction());

    EXPECT_TRUE(Tx.Empty());
    EXPECT_THAT(Tx.NextDue(100000), Not(Optional(_)));
}

TEST(ArqSenderTest, SelectiveAckStopsRetransmissionButIsNotDelivery)
{
    Sender Tx {{.Window = 4, .Capacity = 8}};
    TicketMock Done;
    for (int i = 0; i < 3; ++i) { Tx.Submit(i); }
    Flush(Tx, 0);

    // peer has 1 and 2, still waiting for 0: held behind a gap is not delivered yet, so this
    // must not call Done (a strict mock with no expectation yet)
    Tx.OnAck(0, 0b11, 10, Done.AsStdFunction());
    EXPECT_EQ(Tx.Pending(), 3u);

    // only the missing one goes out again, however long we wait
    EXPECT_THAT(Flush(Tx, 10), ElementsAre(0));
    EXPECT_THAT(Flush(Tx, 100000), ElementsAre(0));

    EXPECT_CALL(Done, Call(_)).Times(3);
    Tx.OnAck(3, 0, 100010, Done.AsStdFunction());
    EXPECT_TRUE(Tx.Empty());
}

TEST(ArqSenderTest, SelectiveAckTriggersOneImmediateResendOfTheGap)
{
    Sender Tx {{.Window = 4, .Capacity = 8}};
    TicketMock Done; // nothing is delivered by a selective ack
    for (int i = 0; i < 3; ++i) { Tx.Submit(i); }
    Flush(Tx, 0);

    // at t=10, long before the timeout, the peer reports it has 2 but not 0 or 1
    Tx.OnAck(0, 0b10, 10, Done.AsStdFunction());
    EXPECT_THAT(Flush(Tx, 10), ElementsAre(0, 1));

    // the same report again must not cause another early resend
    Tx.OnAck(0, 0b10, 20, Done.AsStdFunction());
    EXPECT_THAT(Flush(Tx, 20), IsEmpty());
}

TEST(ArqSenderTest, SelectiveAckDoesNotCutShortTheLastAttempt)
{
    Sender Tx {{.Window = 2, .Capacity = 4, .MaxAttempts = 1}};
    TicketMock Done;
    const auto A = Tx.Submit(0);
    const auto B = Tx.Submit(1);
    Flush(Tx, 0);

    // 1 arrived, 0 has no attempt left: it still waits out its timeout before it is given up on
    Tx.OnAck(0, 0b1, 10, Done.AsStdFunction());
    TicketMock Failed;
    Tx.Abandon(10, Failed.AsStdFunction());

    // Then 0 goes, and 1 with it: it is now the oldest, had its only attempt, and the peer has not
    // confirmed it (it may or may not deliver it once it hears 0 was given up on)
    {
        InSequence Order;
        EXPECT_CALL(Failed, Call(A));
        EXPECT_CALL(Failed, Call(B));
    }
    Tx.Abandon(Rto, Failed.AsStdFunction());
}

TEST(ArqSenderTest, IgnoresAckForFramesNeverSent)
{
    Sender Tx {{.Window = 2, .Capacity = 8}};
    TicketMock Done; // none of these acks may deliver anything
    for (int i = 0; i < 4; ++i) { Tx.Submit(i); }

    Tx.OnAck(1, 0, 0, Done.AsStdFunction()); // nothing has been sent yet

    Flush(Tx, 0);                            // 0 and 1 are out
    Tx.OnAck(3, 0, 0, Done.AsStdFunction()); // claims 2 as well, which is still waiting

    Tx.OnAck(50000, 0, 0, Done.AsStdFunction()); // far behind us
    EXPECT_EQ(Tx.Pending(), 4u);
}

TEST(ArqSenderTest, RepeatedAckIsHarmless)
{
    Sender Tx;
    TicketMock Done;
    Tx.Submit(1);
    Tx.Submit(2);
    Flush(Tx, 0);
    EXPECT_CALL(Done, Call(_)).Times(1); // only the first ack delivers

    Tx.OnAck(1, 0, 10, Done.AsStdFunction());
    Tx.OnAck(1, 0, 11, Done.AsStdFunction());
    Tx.OnAck(0, 0, 12, Done.AsStdFunction()); // older than what we already processed

    EXPECT_EQ(Tx.Pending(), 1u);
}

TEST(ArqSenderTest, GivenUpOnlyAfterTheLastAttemptTimesOut)
{
    Sender Tx {{.Window = 1, .Capacity = 4, .MaxAttempts = 3, .MaxBackoffShift = 0}};
    const auto Id = Tx.Submit(1);
    TicketMock Failed; // strict: nothing may fail before the last attempt has had its full timeout

    uint32_t Now = 0;
    for (int Attempt = 0; Attempt < 3; ++Attempt)
    {
        Tx.Abandon(Now, Failed.AsStdFunction());
        ASSERT_EQ(Flush(Tx, Now).size(), 1u);
        Now += Rto;
    }

    // third attempt went out at 200; it gets its full timeout
    Tx.Abandon(299, Failed.AsStdFunction());
    EXPECT_THAT(Tx.NextDue(299), Not(Optional(_))) << "no fourth attempt";

    EXPECT_CALL(Failed, Call(Id));
    Tx.Abandon(300, Failed.AsStdFunction());
    EXPECT_TRUE(Tx.Empty());
}

TEST(ArqSenderTest, OnlyTheMessageThatRanOutOfAttemptsFailsAndNumberingCarriesOn)
{
    Sender Tx {{.Window = 2, .Capacity = 8, .MaxAttempts = 1, .MaxBackoffShift = 0}};
    TicketMock Failed;
    const auto A = Tx.Submit(1);
    Flush(Tx, 0);
    Tx.Submit(2);
    Flush(Tx, 50); // 1 went out at 0, 2 at 50

    EXPECT_CALL(Failed, Call(A));
    Tx.Abandon(Rto, Failed.AsStdFunction());

    EXPECT_EQ(Tx.Pending(), 1u) << "2 still has its own timeout to wait out";
    EXPECT_THAT(Tx.SkipNotice(), Optional(SeqNum {1})) << "the peer is told to stop waiting for 0";
    Tx.Submit(3);
    EXPECT_THAT(Flush(Tx, Rto), ElementsAre(2)) << "no renumbering: the next message is 2";
}

TEST(ArqSenderTest, SkipNoticeLastsUntilAnAckShowsThePeerHasMovedOn)
{
    Sender Tx {{.Window = 2, .Capacity = 8, .MaxAttempts = 1}};
    NiceMock<MockFunction<void(Sender::Ticket)>> Ignore;
    Tx.Submit(1);
    Flush(Tx, 0);
    EXPECT_FALSE(Tx.SkipNotice().has_value()) << "nothing given up on yet";

    Tx.Abandon(Rto, Ignore.AsStdFunction());
    ASSERT_THAT(Tx.SkipNotice(), Optional(SeqNum {1}));

    Tx.OnAck(0, 0, Rto, Ignore.AsStdFunction()); // the peer has not heard yet
    EXPECT_THAT(Tx.SkipNotice(), Optional(SeqNum {1}));

    Tx.OnAck(1, 0, Rto, Ignore.AsStdFunction()); // now it has
    EXPECT_FALSE(Tx.SkipNotice().has_value());
}

TEST(ArqSenderTest, MessageThePeerHoldsIsOnlySentAgainOnceItIsTheOldest)
{
    Sender Tx {{.Window = 2, .Capacity = 8, .MaxAttempts = 2, .MaxBackoffShift = 0}};
    TicketMock Done;
    TicketMock Failed;
    const auto A = Tx.Submit(1);
    const auto B = Tx.Submit(2);
    Flush(Tx, 0);
    Tx.OnAck(0, 0b1, 10, Done.AsStdFunction()); // the peer holds 1, behind the lost 0

    // while 0 is still being sent, 1 is not: the peer has it
    EXPECT_THAT(Flush(Tx, 10), ElementsAre(0)) << "the gap is resent at once, the held one is not";
    EXPECT_THAT(Flush(Tx, 10 + Rto - 1), IsEmpty());

    // 0 is given up on and 1 is the oldest: sent again, carrying the news that 0 is gone
    EXPECT_CALL(Failed, Call(A));
    Tx.Abandon(10 + Rto, Failed.AsStdFunction());
    EXPECT_THAT(Tx.NextDue(10 + Rto), Optional(AllOf(Field("Seq", &Sender::Frame::Seq, 1), Field("Attempt", &Sender::Frame::Attempt, 1))));

    EXPECT_CALL(Done, Call(B));
    Tx.OnAck(2, 0, 10 + Rto, Done.AsStdFunction()); // the peer skipped 0 and delivered 1
    EXPECT_TRUE(Tx.Empty());
}

TEST(ArqSenderTest, MessageSubmittedFromFailCallbackSurvives)
{
    Sender Tx {{.Window = 2, .Capacity = 8, .MaxAttempts = 1}};
    Tx.Submit(1);
    Tx.Submit(2);
    Flush(Tx, 0);

    // each failure reported resubmits a new message from inside the callback
    int Failures = 0;
    TicketMock Failed;
    EXPECT_CALL(Failed, Call(_)).Times(2).WillRepeatedly([&](Sender::Ticket) { Tx.Submit(100 + ++Failures); });

    Tx.Abandon(Rto, Failed.AsStdFunction());

    EXPECT_EQ(Tx.Pending(), 2u);
    EXPECT_THAT(Tx.NextDue(Rto),
                Optional(AllOf(Field("Seq", &Sender::Frame::Seq, 2),
                               Field("Payload", &Sender::Frame::Payload, testing::Pointee(101)))));
}

TEST(ArqSenderTest, RestartKeepsMessagesAndRenumbersFromZero)
{
    Sender Tx {{.Window = 4, .Capacity = 8}};
    NiceMock<MockFunction<void(Sender::Ticket)>> Done;
    for (int i = 0; i < 4; ++i) { Tx.Submit(i); }
    Flush(Tx, 0);
    Tx.OnAck(2, 0b1, 10, Done.AsStdFunction()); // 0 and 1 delivered, 3 held by the peer
    ASSERT_EQ(Tx.Pending(), 2u);

    Tx.Restart();
    EXPECT_FALSE(Tx.SkipNotice().has_value()) << "a peer that starts over is waiting for nothing";

    // both remaining messages go out again, including the one the old peer was holding
    EXPECT_THAT(Tx.NextDue(10),
                Optional(AllOf(Field("Seq", &Sender::Frame::Seq, 0),
                               Field("Attempt", &Sender::Frame::Attempt, 0),
                               Field("Payload", &Sender::Frame::Payload, testing::Pointee(2)))));
    EXPECT_THAT(Flush(Tx, 10), ElementsAre(0, 1));
}

// ---------------------------------------------------------------------------
// Both halves over a bad link
// ---------------------------------------------------------------------------

namespace
{
struct LinkParams
{
    uint16_t SenderWindow;
    uint16_t ReceiverWindow;
    uint32_t LossPercent;
    uint32_t DupPercent;
    uint32_t MaxDelayMs; // random per frame, so frames overtake each other
    uint32_t Seed;
};

// What a run gave up on: its tickets, which are 1 + the message's number since tickets count from 1
struct GivenUp
{
    std::vector<Sender::Ticket> Delivered;
    std::vector<Sender::Ticket> Failed;
    std::vector<Sender::Ticket> InOrder; // both together, in the order the sender reported them
};

// Push Count messages through a lossy, duplicating, reordering link and return what came out.
// With MaxAttempts below 255 messages are given up on, and the sender's skip notice rides on its
// data frames as Radio does, plus a notice of its own every 50 ms while it is pending.
std::vector<int> RunLink(const LinkParams& P,
                         int Count,
                         uint32_t& OutFramesSent,
                         uint8_t MaxAttempts = 255,
                         GivenUp* Outcomes  = nullptr)
{
    struct DataFrame
    {
        uint32_t ArrivesMs;
        SeqNum Seq;
        int Payload;
        std::optional<SeqNum> Oldest; // the skip notice it carries
        bool NoticeOnly = false;
    };
    struct AckFrame
    {
        uint32_t ArrivesMs;
        SeqNum Next;
        uint16_t Sack;
    };

    Sender Tx {{.Window = P.SenderWindow, .Capacity = 16, .MaxAttempts = MaxAttempts}};
    Receiver Rx {{.Window = P.ReceiverWindow}};
    std::mt19937 Rng {P.Seed};
    std::vector<DataFrame> Forward;
    std::vector<AckFrame> Backward;
    Sink Out;
    Tickets Done;
    Tickets Failed;
    std::vector<Sender::Ticket> Reported;
    const auto OnDone = [&](Sender::Ticket Id)
    {
        Done.Items.push_back(Id);
        Reported.push_back(Id);
    };
    const auto OnFailed = [&](Sender::Ticket Id)
    {
        Failed.Items.push_back(Id);
        Reported.push_back(Id);
    };

    const auto Lost  = [&] { return (Rng() % 100) < P.LossPercent; };
    const auto Delay = [&] { return 1 + (P.MaxDelayMs ? Rng() % P.MaxDelayMs : 0); };

    int Submitted = 0;
    OutFramesSent = 0;

    for (uint32_t Now = 0; Now < 4'000'000; ++Now)
    {
        while (Submitted < Count && !Tx.Full()) { Tx.Submit(Submitted++); }
        Tx.Abandon(Now, OnFailed);

        while (const auto Due = Tx.NextDue(Now))
        {
            ++OutFramesSent;
            const int Copies = ((Rng() % 100) < P.DupPercent) ? 2 : 1;
            for (int i = 0; i < Copies; ++i)
            {
                if (!Lost()) { Forward.push_back({Now + Delay(), Due->Seq, *Due->Payload, Tx.SkipNotice()}); }
            }
            Tx.MarkSent(Due->Seq, Now, Rto);
        }
        if (Tx.SkipNotice() && Now % 50 == 0 && !Lost())
        {
            Forward.push_back({Now + Delay(), 0, 0, Tx.SkipNotice(), true});
        }

        for (size_t i = 0; i < Forward.size();)
        {
            if (Forward[i].ArrivesMs > Now)
            {
                ++i;
                continue;
            }
            if (Forward[i].Oldest) { Rx.OnSkip(*Forward[i].Oldest, Out.Fn()); }
            if (!Forward[i].NoticeOnly) { Rx.OnFrame(Forward[i].Seq, Forward[i].Payload, Out.Fn()); }
            Forward.erase(Forward.begin() + static_cast<std::ptrdiff_t>(i));
        }

        if (Rx.AckPending())
        {
            if (!Lost()) { Backward.push_back({Now + Delay(), Rx.AckNext(), Rx.AckSack()}); }
            Rx.AckSent();
        }

        for (size_t i = 0; i < Backward.size();)
        {
            if (Backward[i].ArrivesMs > Now)
            {
                ++i;
                continue;
            }
            Tx.OnAck(Backward[i].Next, Backward[i].Sack, Now, OnDone);
            Backward.erase(Backward.begin() + static_cast<std::ptrdiff_t>(i));
        }

        if (Submitted == Count && Tx.Empty()) { break; }
    }

    EXPECT_EQ(Done.Items.size() + Failed.Items.size(), static_cast<size_t>(Count)) << "every message needs an outcome";
    if (Outcomes != nullptr) { *Outcomes = {.Delivered = Done.Items, .Failed = Failed.Items, .InOrder = Reported}; }
    else { EXPECT_THAT(Failed.Items, IsEmpty()) << "every message must be reported delivered"; }
    return Out.Items;
}

std::vector<int> Iota(int Count)
{
    std::vector<int> Out(static_cast<size_t>(Count));
    for (int i = 0; i < Count; ++i) { Out[static_cast<size_t>(i)] = i; }
    return Out;
}
} // namespace

class ArqLinkTest : public testing::TestWithParam<LinkParams>
{
};

TEST_P(ArqLinkTest, EveryMessageArrivesExactlyOnceInOrder)
{
    constexpr int Count = 2000;
    uint32_t FramesSent = 0;

    const auto Received = RunLink(GetParam(), Count, FramesSent);

    EXPECT_THAT(Received, testing::ElementsAreArray(Iota(Count)));
}

// The conditions below, in the same order
std::string ConditionName(const testing::TestParamInfo<LinkParams>& Info)
{
    static constexpr const char* Names[] = {"CleanLink",
                                            "HeavyLossBothWays",
                                            "DuplicatesAndReordering",
                                            "EverythingAtOnce",
                                            "StopAndWait",
                                            "SenderWindowLargerThanReceivers",
                                            "WidestReceiverWindow"};
    return Names[Info.index];
}

INSTANTIATE_TEST_SUITE_P(
    Conditions,
    ArqLinkTest,
    testing::Values(
        // clean link
        LinkParams {.SenderWindow = 4, .ReceiverWindow = 8, .LossPercent = 0, .DupPercent = 0, .MaxDelayMs = 0, .Seed = 1},
        // heavy loss both ways
        LinkParams {.SenderWindow = 4, .ReceiverWindow = 8, .LossPercent = 40, .DupPercent = 0, .MaxDelayMs = 0, .Seed = 2},
        // duplicates and reordering (delay longer than the retransmit timeout)
        LinkParams {.SenderWindow = 4, .ReceiverWindow = 8, .LossPercent = 0, .DupPercent = 30, .MaxDelayMs = 250, .Seed = 3},
        // everything at once
        LinkParams {.SenderWindow = 8, .ReceiverWindow = 8, .LossPercent = 25, .DupPercent = 20, .MaxDelayMs = 250, .Seed = 4},
        // stop and wait
        LinkParams {.SenderWindow = 1, .ReceiverWindow = 1, .LossPercent = 30, .DupPercent = 10, .MaxDelayMs = 50, .Seed = 5},
        // sender window larger than the receiver's: the excess is dropped and resent
        LinkParams {.SenderWindow = 8, .ReceiverWindow = 2, .LossPercent = 20, .DupPercent = 0, .MaxDelayMs = 50, .Seed = 6},
        // widest receiver window
        LinkParams {.SenderWindow = 16, .ReceiverWindow = 17, .LossPercent = 30, .DupPercent = 10, .MaxDelayMs = 300, .Seed = 7}),
    ConditionName);

TEST(ArqLinkTest, SequenceNumbersWrapUnderLoss)
{
    // more messages than there are sequence numbers
    constexpr int Count = 70000;
    uint32_t FramesSent = 0;

    const auto Received = RunLink(
        {.SenderWindow = 8, .ReceiverWindow = 8, .LossPercent = 10, .DupPercent = 5, .MaxDelayMs = 20, .Seed = 8},
        Count,
        FramesSent);

    EXPECT_THAT(Received, testing::ElementsAreArray(Iota(Count)));
}

TEST(ArqLinkTest, CleanLinkSendsEachMessageOnce)
{
    uint32_t FramesSent = 0;

    RunLink({.SenderWindow = 4, .ReceiverWindow = 8, .LossPercent = 0, .DupPercent = 0, .MaxDelayMs = 0, .Seed = 9},
            500,
            FramesSent);

    EXPECT_EQ(FramesSent, 500u) << "no spurious retransmissions";
}

// Few attempts on a bad link, so messages are given up on all the time. The rest still arrive once
// each and in order, every message has exactly one outcome, and none reported delivered is missing.
class ArqGiveUpTest : public testing::TestWithParam<LinkParams>
{
};

TEST_P(ArqGiveUpTest, MessagesGivenUpOnAreSkippedAndTheRestArriveOnceInOrder)
{
    constexpr int Count = 2000;
    uint32_t FramesSent = 0;
    GivenUp Outcomes;

    const auto Received = RunLink(GetParam(), Count, FramesSent, 3, &Outcomes);

    EXPECT_FALSE(Outcomes.Failed.empty()) << "the run must actually give up on some";
    EXPECT_TRUE(std::adjacent_find(Received.begin(), Received.end(), [](int A, int B) { return B <= A; }) == Received.end())
        << "once each and in order";
    for (const auto Id : Outcomes.Delivered)
    {
        EXPECT_TRUE(std::binary_search(Received.begin(), Received.end(), static_cast<int>(Id) - 1))
            << "reported delivered but never received: message " << Id - 1;
    }
}

INSTANTIATE_TEST_SUITE_P(
    LossyLinks,
    ArqGiveUpTest,
    testing::Values(
        LinkParams {.SenderWindow = 4, .ReceiverWindow = 8, .LossPercent = 40, .DupPercent = 0, .MaxDelayMs = 0, .Seed = 21},
        LinkParams {.SenderWindow = 4, .ReceiverWindow = 8, .LossPercent = 50, .DupPercent = 20, .MaxDelayMs = 250, .Seed = 22},
        LinkParams {.SenderWindow = 8, .ReceiverWindow = 4, .LossPercent = 40, .DupPercent = 10, .MaxDelayMs = 300, .Seed = 23},
        LinkParams {.SenderWindow = 16, .ReceiverWindow = 17, .LossPercent = 60, .DupPercent = 10, .MaxDelayMs = 100, .Seed = 24},
        LinkParams {.SenderWindow = 1, .ReceiverWindow = 1, .LossPercent = 50, .DupPercent = 0, .MaxDelayMs = 50, .Seed = 25}));

// Radio::TakeTextPart relies on this: the parts of the oldest reliable text are the only ones that
// can be reported, because outcomes come in the order the messages were submitted. Delivered and
// failed alike, even with acks arriving late, out of order and in part, and messages given up on.
TEST(ArqSenderTest, OutcomesAreReportedInTheOrderTheMessagesWereSubmitted)
{
    constexpr int Count = 500;

    for (uint32_t Seed = 1; Seed <= 20; ++Seed)
    {
        uint32_t FramesSent = 0;
        GivenUp Outcomes;
        RunLink({.SenderWindow = static_cast<uint16_t>(1 + Seed % 16),
                 .ReceiverWindow = 8,
                 .LossPercent    = 30 + Seed % 4 * 10,
                 .DupPercent     = 10,
                 .MaxDelayMs     = 250,
                 .Seed           = 100 + Seed},
                Count,
                FramesSent,
                3,
                &Outcomes);

        ASSERT_FALSE(Outcomes.Failed.empty()) << "seed " << Seed << ": the run must actually give up on some";
        ASSERT_FALSE(Outcomes.Delivered.empty()) << "seed " << Seed;

        // Tickets are handed out one after the other, so every ticket exactly once, in order,
        // is the same as the list counting 1, 2, 3 ... with no gap, repeat or swap
        std::vector<Sender::Ticket> Expected(Count);
        for (int i = 0; i < Count; ++i) { Expected[static_cast<size_t>(i)] = static_cast<Sender::Ticket>(i + 1); }
        EXPECT_EQ(Outcomes.InOrder, Expected) << "seed " << Seed;
    }
}

// ---------------------------------------------------------------------------
// Frames that wait in a transport queue before going on air
// ---------------------------------------------------------------------------

TEST(ArqSenderTest, QueuedFrameOnlyTimesOutOnceItHasLeft)
{
    Sender Tx;
    Tx.Submit(1);
    ASSERT_TRUE(Tx.NextDue(0).has_value());
    Tx.MarkQueued(0, 0, 5000, 2, 7);

    // far past the retransmit timeout, but it has not gone out yet
    EXPECT_THAT(Tx.NextDue(Rto * 10), Not(Optional(_)));

    // The sender asks the transport (Via 2, Mark 7) how long ago the frame left, and only about
    // that frame, only while it has not left, and not again once it has: a third question
    // would be a call no expectation allows.
    StrictMock<MockFunction<uint32_t(uint8_t, uint32_t)>> Left;
    {
        InSequence Order;
        EXPECT_CALL(Left, Call(2, 7)).WillOnce(Return(0)); // still waiting
        EXPECT_CALL(Left, Call(2, 7)).WillOnce(Return(Rto));
    }

    Tx.StartTimers(1000, Left.AsStdFunction());
    EXPECT_THAT(Tx.NextDue(1000 + Rto), Not(Optional(_)));

    // it leaves at 2000: the timeout runs from there
    Tx.StartTimers(2000, Left.AsStdFunction());
    EXPECT_THAT(Tx.NextDue(2000 + Rto - 1), Not(Optional(_)));

    EXPECT_THAT(Tx.NextDue(2000 + Rto), Optional(Field("Attempt", &Sender::Frame::Attempt, 1)));

    // once started it is not asked about again
    Tx.StartTimers(2001, Left.AsStdFunction());
}

TEST(ArqSenderTest, QueuedFrameThatNeverLeavesIsResentAfterTheStallLimit)
{
    Sender Tx;
    Tx.Submit(1);
    Tx.MarkQueued(0, 0, 5000, 0, 1);

    EXPECT_THAT(Tx.NextDue(4999), Not(Optional(_)));
    EXPECT_THAT(Tx.NextDue(5000), Optional(Field("Attempt", &Sender::Frame::Attempt, 1)));
}

TEST(ArqSenderTest, BackoffStillDoublesForQueuedFrames)
{
    Sender Tx;
    Tx.Submit(1);
    NiceMock<MockFunction<uint32_t(uint8_t, uint32_t)>> Leaves;
    EXPECT_CALL(Leaves, Call(_, _)).WillRepeatedly(Return(Rto)); // the frame leaves at once

    Tx.MarkQueued(0, 0, 5000, 0, 1);
    Tx.StartTimers(0, Leaves.AsStdFunction());
    ASSERT_TRUE(Tx.NextDue(Rto).has_value());

    // second attempt waits twice as long
    Tx.MarkQueued(0, Rto, 5000, 0, 2);
    Tx.StartTimers(Rto, Leaves.AsStdFunction());
    EXPECT_THAT(Tx.NextDue(Rto + 2 * Rto - 1), Not(Optional(_)));
    EXPECT_THAT(Tx.NextDue(Rto + 2 * Rto), Optional(_));
}

TEST(ArqSenderTest, FrameStillQueuedIsNotFastResent)
{
    Sender Tx {{.Window = 4, .Capacity = 8}};
    NiceMock<MockFunction<void(Sender::Ticket)>> Done;
    Tx.Submit(1);
    Tx.Submit(2);
    Tx.MarkQueued(0, 0, 5000, 0, 1); // waiting in a queue
    Tx.MarkSent(1, 0, Rto);          // went out on another transport and arrived

    // the peer holds seq 1 and is missing seq 0
    Tx.OnAck(0, 0b1, 10, Done.AsStdFunction());

    EXPECT_THAT(Tx.NextDue(10), Not(Optional(_))) << "seq 0 was not lost, it has not been sent yet";
}

// ---------------------------------------------------------------------------
// A message the peer holds, when the peer then falls silent
// ---------------------------------------------------------------------------

TEST(ArqSenderTest, MessagesBehindAHeldOldestStillFailWhenThePeerFallsSilent)
{
    Sender Tx {{.Window = 2, .Capacity = 8, .MaxAttempts = 1, .MaxBackoffShift = 0}};
    NiceMock<MockFunction<void(Sender::Ticket)>> Done;
    const auto A = Tx.Submit(1);
    const auto B = Tx.Submit(2);
    const auto C = Tx.Submit(3);
    ASSERT_THAT(Flush(Tx, 0), ElementsAre(0, 1));
    Tx.OnAck(0, 0b1, 10, Done.AsStdFunction()); // the peer holds 1, 0 was lost; then it is never heard again

    TicketMock Failed;
    {
        InSequence Order;
        EXPECT_CALL(Failed, Call(A));
        EXPECT_CALL(Failed, Call(B)); // the oldest now, its one attempt spent, never confirmed
    }
    Tx.Abandon(Rto, Failed.AsStdFunction());
    ASSERT_THAT(Flush(Tx, Rto), ElementsAre(2)); // C goes out once
    Mock::VerifyAndClearExpectations(&Failed);

    EXPECT_CALL(Failed, Call(C)); // nothing may be left without an outcome
    Tx.Abandon(1000000, Failed.AsStdFunction());
    EXPECT_TRUE(Tx.Empty());
}

// Giving up and skipping across the 16 bit sequence wrap
TEST(ArqLinkTest, MessagesGivenUpOnAreSkippedAcrossTheSequenceWrap)
{
    constexpr int Count = 70000;
    uint32_t FramesSent = 0;
    GivenUp Outcomes;

    const auto Received = RunLink(
        {.SenderWindow = 8, .ReceiverWindow = 8, .LossPercent = 40, .DupPercent = 5, .MaxDelayMs = 20, .Seed = 31},
        Count,
        FramesSent,
        3,
        &Outcomes);

    EXPECT_FALSE(Outcomes.Failed.empty());
    EXPECT_GT(Received.size(), 65536u) << "the run must get past the wrap";
    EXPECT_TRUE(std::adjacent_find(Received.begin(), Received.end(), [](int A, int B) { return B <= A; }) == Received.end())
        << "once each and in order";
}
