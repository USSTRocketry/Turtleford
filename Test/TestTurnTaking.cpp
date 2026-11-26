#include <gtest/gtest.h>

#include "Transport/TurnTaking.h"

// The turn taking rules without a radio. The same rules seen through LoraTransport are in
// TestLoraTransport.cpp (LoraTurnTakingTest); these pin down the cases that are hard to reach
// from there: two machines active at once, several transitions in one Advance(), and what is
// ignored in which state.

namespace ra::turtleford
{
namespace
{
// ListenEveryMs 100 (held for 200 at most), window 100 to 124, quiet after 125, no guard
TurnTakingConfig Config(uint32_t GuardMs = 0, uint32_t ListenMs = 100)
{
    return TurnTakingConfig {.ListenEveryMs = 100, .ListenMs = ListenMs, .GuardMs = GuardMs, .QuietMs = 125, .Seed = 1};
}

// The peer finishes a message at NowMs; a frame of it was heard then too, as LoraTransport reports it
void PeerMessage(TurnTaking& T, uint32_t NowMs, bool Over, bool Bulk = false)
{
    T.Heard(NowMs);
    T.PeerFinished(NowMs, Over, Bulk);
}
} // namespace

TEST(TurnTakingRulesTest, FreeToTransmitAtStart)
{
    TurnTaking T {Config()};
    EXPECT_TRUE(T.Advance(0));
    EXPECT_TRUE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, MarkedMessageOpensAWindowAtTheNextAdvanceThatEndsAtItsDeadline)
{
    TurnTaking T {Config()};
    T.Sent(true);
    EXPECT_FALSE(T.MayTransmit());

    EXPECT_TRUE(T.Advance(1000));
    EXPECT_FALSE(T.MayTransmit());
    EXPECT_TRUE(T.Advance(1099));
    EXPECT_FALSE(T.MayTransmit());

    // The window is ListenMs plus less than a quarter more
    EXPECT_TRUE(T.Advance(1125));
    EXPECT_TRUE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, UnmarkedMessageOpensNoWindow)
{
    TurnTaking T {Config()};
    T.Sent(false);
    EXPECT_TRUE(T.Advance(0));
    EXPECT_TRUE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, ZeroListenMsOpensAndClosesTheWindowInOneAdvance)
{
    TurnTaking T {Config(0, 0)};
    T.Sent(true);

    // WindowPending -> Listening -> Free
    EXPECT_TRUE(T.Advance(0));
    EXPECT_TRUE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, PeerOverEndsTheWindowAtOnce)
{
    TurnTaking T {Config()};
    T.Sent(true);
    EXPECT_TRUE(T.Advance(0));

    PeerMessage(T, 10, true);
    EXPECT_TRUE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, PeerOverBeforeTheWindowHasStartedDoesNotCancelIt)
{
    TurnTaking T {Config()};
    T.Sent(true);
    PeerMessage(T, 0, true);

    EXPECT_TRUE(T.Advance(0));
    EXPECT_FALSE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, PeerGoingOnDuringTheGuardKeepsUsHoldingAfterTheGuardEnds)
{
    TurnTaking T {Config(10)};
    PeerMessage(T, 0, true);
    PeerMessage(T, 5, false);

    EXPECT_TRUE(T.Advance(10)); // guard over, still holding
    EXPECT_FALSE(T.MayTransmit());

    EXPECT_TRUE(T.Advance(5 + 125)); // quiet since its last frame
    EXPECT_TRUE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, GuardLongerThanAStreamSlotStillHoldsUsAfterThePeerGoesQuiet)
{
    TurnTaking T {Config(200)};
    PeerMessage(T, 0, true, true); // slot until 50, guard until 200

    EXPECT_TRUE(T.Advance(50)); // the stream resumes: holding and guarding at once
    EXPECT_FALSE(T.MayTransmit());

    EXPECT_TRUE(T.Advance(130)); // no longer holding (quiet), still guarding
    EXPECT_FALSE(T.MayTransmit());

    EXPECT_TRUE(T.Advance(200));
    EXPECT_TRUE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, BulkOverFreesUsForHalfAListenMsAndThenHolds)
{
    TurnTaking T {Config()};
    PeerMessage(T, 0, true, true);
    EXPECT_TRUE(T.MayTransmit());

    EXPECT_TRUE(T.Advance(49));
    EXPECT_TRUE(T.MayTransmit());
    EXPECT_TRUE(T.Advance(50));
    EXPECT_FALSE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, SlotThatEndsAfterThePeerWentQuietGoesThroughHoldingToFreeInOneAdvance)
{
    TurnTaking T {Config()};
    PeerMessage(T, 0, true, true);

    // Slot -> Holding at 130, which is already quiet -> Free
    EXPECT_TRUE(T.Advance(130));
    EXPECT_TRUE(T.MayTransmit());

    // Free, not given up: the next message without an over holds us again
    PeerMessage(T, 140, false);
    EXPECT_FALSE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, QuietIsCheckedBeforeGivingUp)
{
    TurnTaking T {Config()};
    PeerMessage(T, 0, false);

    // Both quiet (250 >= 125) and held too long (250 >= 200): quiet wins, so not given up
    EXPECT_TRUE(T.Advance(250));
    EXPECT_TRUE(T.MayTransmit());

    PeerMessage(T, 260, false);
    EXPECT_FALSE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, MoreFromThePeerWhileHoldingDoesNotRestartTheHold)
{
    TurnTaking T {Config()};
    PeerMessage(T, 0, false);
    PeerMessage(T, 150, false);

    // Held since 0, not 150: given up at 200, while the peer is not yet quiet
    EXPECT_TRUE(T.Advance(200));
    EXPECT_TRUE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, AfterGivingUpOnlyAnOverMakesUsHoldAgain)
{
    TurnTaking T {Config()};
    PeerMessage(T, 0, false);
    EXPECT_TRUE(T.Advance(10));
    PeerMessage(T, 100, false);
    EXPECT_TRUE(T.Advance(200)); // given up
    ASSERT_TRUE(T.MayTransmit());

    PeerMessage(T, 210, false);
    EXPECT_TRUE(T.MayTransmit());

    PeerMessage(T, 220, true);
    PeerMessage(T, 230, false);
    EXPECT_FALSE(T.MayTransmit());
}

TEST(TurnTakingRulesTest, NonBulkMessageIsMarkedOverOnlyWhenItIsTheLastQueued)
{
    TurnTaking T {Config()};
    EXPECT_FALSE(T.MarkOver(0, false, false));
    EXPECT_TRUE(T.MarkOver(0, false, true));
}

TEST(TurnTakingRulesTest, BulkMessageIsMarkedOverOncePerListenEveryMsCountedFromAnyOver)
{
    TurnTaking T {Config()};
    EXPECT_TRUE(T.MarkOver(0, true, false));
    EXPECT_FALSE(T.MarkOver(50, true, false));
    EXPECT_TRUE(T.MarkOver(100, true, false));

    // A non-bulk over restarts the period too
    EXPECT_TRUE(T.MarkOver(150, false, true));
    EXPECT_FALSE(T.MarkOver(200, true, false));
    EXPECT_TRUE(T.MarkOver(250, true, false));
}

TEST(TurnTakingRulesTest, ZeroListenEveryMsMarksNothingAndNeverWaits)
{
    TurnTaking T {TurnTakingConfig {.ListenEveryMs = 0}};
    EXPECT_FALSE(T.Enabled());
    EXPECT_FALSE(T.MarkOver(0, true, true));

    T.Sent(true);
    PeerMessage(T, 0, false);
    EXPECT_TRUE(T.Advance(0));
    EXPECT_TRUE(T.MayTransmit());
}
} // namespace ra::turtleford
