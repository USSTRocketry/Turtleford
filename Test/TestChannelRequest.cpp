// The master's channel request bookkeeping on its own: no Radio, no link, no clock.

#include <gtest/gtest.h>

#include <cstdint>

#include "Link/ChannelRequest.h"

using namespace ra::turtleford;
using Next = ChannelRequest::Next;

namespace
{
constexpr ChannelNumber Old    = 0;
constexpr ChannelNumber Wanted = 2;
constexpr uint32_t First       = 7;
} // namespace

TEST(ChannelRequestTest, IsNotActiveByDefaultAndEverySettleIsReported)
{
    ChannelRequest Request;

    EXPECT_FALSE(Request.Active());
    EXPECT_FALSE(Request.SubmitOwed());
    EXPECT_EQ(Request.OnSettled(Old), Next::Report);
    EXPECT_EQ(Request.OnSettled(Wanted), Next::Report);
    EXPECT_FALSE(Request.Active());
}

TEST(ChannelRequestTest, BeginMakesItActiveAndOwnsOnlyTheTicketItWasGiven)
{
    ChannelRequest Request;
    Request.Begin(Wanted, 2, First);

    EXPECT_TRUE(Request.Active());
    EXPECT_EQ(Request.Wanted(), Wanted);
    EXPECT_FALSE(Request.SubmitOwed());
    EXPECT_TRUE(Request.Owns(First));
    EXPECT_FALSE(Request.Owns(First + 1));
    EXPECT_FALSE(Request.Owns(ChannelRequest::NoTicket));
}

TEST(ChannelRequestTest, NoTicketIsNeverOwnedEvenOnceTheRequestHasBeenAnswered)
{
    ChannelRequest Request;
    EXPECT_FALSE(Request.Owns(ChannelRequest::NoTicket));

    Request.Begin(Wanted, 0, First);
    Request.Answered();

    EXPECT_FALSE(Request.Owns(ChannelRequest::NoTicket));
    EXPECT_FALSE(Request.Owns(First));
    EXPECT_TRUE(Request.Active()) << "answered is not finished: the link still has to settle";
}

TEST(ChannelRequestTest, SettlingWhileTheRequestIsOnItsWayIsNotItsOutcome)
{
    ChannelRequest Request;
    Request.Begin(Wanted, 2, First);

    EXPECT_EQ(Request.OnSettled(Old), Next::Wait);
    EXPECT_EQ(Request.OnSettled(Wanted), Next::Wait);
    EXPECT_TRUE(Request.Active());
    EXPECT_TRUE(Request.Owns(First));
}

TEST(ChannelRequestTest, SettlingOnTheWantedChannelAfterTheAnswerReportsAndFinishes)
{
    ChannelRequest Request;
    Request.Begin(Wanted, 2, First);
    Request.Answered();

    EXPECT_EQ(Request.OnSettled(Wanted), Next::Report);
    EXPECT_FALSE(Request.Active());
    EXPECT_EQ(Request.OnSettled(Old), Next::Report) << "nothing is being worked on any more";
}

TEST(ChannelRequestTest, SettlingElsewhereWithRetriesLeftAsksAgainAndWaitsForTheNewTicket)
{
    ChannelRequest Request;
    Request.Begin(Wanted, 2, First);
    Request.Answered();

    EXPECT_EQ(Request.OnSettled(Old), Next::Retry);
    EXPECT_TRUE(Request.Active());
    EXPECT_TRUE(Request.SubmitOwed());
    EXPECT_EQ(Request.Wanted(), Wanted);
    EXPECT_EQ(Request.OnSettled(Old), Next::Wait) << "the retry has not been accepted yet";
    EXPECT_TRUE(Request.SubmitOwed());

    Request.Submitted(First + 1);
    EXPECT_FALSE(Request.SubmitOwed());
    EXPECT_TRUE(Request.Owns(First + 1));
    EXPECT_FALSE(Request.Owns(First));
    EXPECT_EQ(Request.OnSettled(Old), Next::Wait) << "the retry is on its way";
}

TEST(ChannelRequestTest, RetriesAreCountedDownExactlyThenTheWrongChannelIsReported)
{
    constexpr uint8_t Retries = 3;
    ChannelRequest Request;
    Request.Begin(Wanted, Retries, First);
    Request.Answered();

    for (uint8_t Attempt = 0; Attempt < Retries; ++Attempt)
    {
        ASSERT_EQ(Request.OnSettled(Old), Next::Retry) << "retry " << int {Attempt};
        Request.Submitted(First + 1 + Attempt);
        Request.Answered();
    }

    EXPECT_EQ(Request.OnSettled(Old), Next::Report);
    EXPECT_FALSE(Request.Active());
}

TEST(ChannelRequestTest, ARetryThatLandsOnTheWantedChannelEndsTheRequestWithRetriesToSpare)
{
    ChannelRequest Request;
    Request.Begin(Wanted, 5, First);
    Request.Answered();
    ASSERT_EQ(Request.OnSettled(Old), Next::Retry);
    Request.Submitted(First + 1);
    Request.Answered();

    EXPECT_EQ(Request.OnSettled(Wanted), Next::Report);
    EXPECT_FALSE(Request.Active());
}

TEST(ChannelRequestTest, ZeroRetriesReportsTheFirstSettleOnTheWrongChannel)
{
    ChannelRequest Request;
    Request.Begin(Wanted, 0, First);
    Request.Answered();

    EXPECT_EQ(Request.OnSettled(Old), Next::Report);
    EXPECT_FALSE(Request.Active());
}

TEST(ChannelRequestTest, BeginStartsOverAfterAFinishedRequest)
{
    ChannelRequest Request;
    Request.Begin(Wanted, 0, First);
    Request.Answered();
    ASSERT_EQ(Request.OnSettled(Old), Next::Report);

    Request.Begin(1, 1, First + 10);
    EXPECT_TRUE(Request.Active());
    EXPECT_EQ(Request.Wanted(), 1u);
    EXPECT_TRUE(Request.Owns(First + 10));
    EXPECT_FALSE(Request.Owns(First));
}
