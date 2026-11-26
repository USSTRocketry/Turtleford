#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <tuple>
#include <vector>

#include "Link/Link.h"
#include "Mocks/MockTransport.h"
#include "Sim/SimTransport.h"
#include "Transport/Transport.h"

using namespace ra::turtleford;
using mocks::MockTransport;
using testing::AllOf;
using testing::Mock;
using testing::Not;
using testing::NiceMock;
using testing::_;

namespace ra::turtleford
{
// So a failed state check says "Searching" and not "3"
void PrintTo(Link::State State, std::ostream* Out)
{
    switch (State)
    {
        case Link::State::Stable: *Out << "Stable"; break;
        case Link::State::SwitchPending: *Out << "SwitchPending"; break;
        case Link::State::Confirming: *Out << "Confirming"; break;
        case Link::State::Searching: *Out << "Searching"; break;
    }
}
} // namespace ra::turtleford

namespace
{
constexpr uint32_t LostMs    = 1000;
constexpr uint32_t ConfirmMs = 400;
constexpr uint32_t DwellMs   = 200;

LinkConfig LinkTestConfig(bool Master)
{
    LinkConfig Out;
    Out.LostTimeoutMs    = LostMs;
    Out.ConfirmTimeoutMs = ConfirmMs;
    Out.SearchDwellMs    = DwellMs;
    Out.ChannelMaster    = Master;
    return Out;
}

// What a call on the link reported
MATCHER(WentUp, "reports the link going up") { return arg.WentUp; }
MATCHER(WentDown, "reports the link going down") { return arg.WentDown; }
MATCHER(Settled, "reports the channel settled") { return arg.ChannelSettled; }
MATCHER(Probes, "asks for a probe frame") { return arg.Probe; }

class LinkTest : public testing::Test
{
protected:
    // The link under test. The transport is put on its starting channel first, because that
    // becomes the link's home channel.
    void Start(bool Master, uint8_t StartChannel = 0)
    {
        EXPECT_TRUE(Sim.SetChannel(StartChannel));
        L.emplace(T, LinkTestConfig(Master));
    }

    // Agree on a move to Channel and retune at StartMs. Leaves the link in Confirming.
    void SwitchTo(ChannelNumber Channel, uint32_t StartMs)
    {
        ASSERT_TRUE(L->BeginSwitch(Channel));
        ASSERT_THAT(L->Poll(StartMs, true), Probes());
        ASSERT_EQ(L->GetState(), Link::State::Confirming);
        ASSERT_EQ(L->Channel(), Channel);
    }

    // The simulated transport (4 channels, on 0) does the work. Wrapped in a mock, so the tests
    // that are about what the link asks of its transport can say so.
    sim::SimTransport Sim;
    NiceMock<MockTransport> T {Sim};
    std::optional<Link> L;
};
} // namespace

// ---- reachability -----------------------------------------------------------

TEST_F(LinkTest, StartsStableAndDownOnTheTransportsChannel)
{
    Start(false, 2);

    EXPECT_EQ(L->GetState(), Link::State::Stable);
    EXPECT_FALSE(L->IsUp());
    EXPECT_FALSE(L->Holding());
    EXPECT_TRUE(L->HasChannels());
    EXPECT_EQ(L->HomeChannel(), 2);
    EXPECT_EQ(L->Channel(), 2);
    EXPECT_EQ(&L->Transport(), &T);
}

TEST_F(LinkTest, PollBeforeAnythingWasHeardReportsNothing)
{
    Start(false);

    const auto Events = L->Poll(LostMs * 5, true);

    EXPECT_THAT(Events, Not(WentUp()));
    EXPECT_THAT(Events, Not(WentDown()));
    EXPECT_THAT(Events, Not(Settled()));
    EXPECT_THAT(Events, Not(Probes()));
    EXPECT_EQ(L->GetState(), Link::State::Stable);
}

TEST_F(LinkTest, OnHeardReportsWentUpOnce)
{
    Start(false);

    const auto First = L->OnHeard(100);
    EXPECT_THAT(First, WentUp());
    EXPECT_THAT(First, Not(WentDown()));
    EXPECT_THAT(First, Not(Settled())) << "nothing was in progress";
    EXPECT_THAT(First, Not(Probes()));
    EXPECT_TRUE(L->IsUp());

    const auto Second = L->OnHeard(200);
    EXPECT_THAT(Second, Not(WentUp()));
    EXPECT_TRUE(L->IsUp());
}

TEST_F(LinkTest, IsNotUpWhileTheTransportIsNotReady)
{
    Start(false);
    L->OnHeard(100);
    ASSERT_TRUE(L->IsUp());

    Sim.Ready = false; // e.g. BLE disconnected
    EXPECT_FALSE(L->IsUp());

    Sim.Ready = true;
    EXPECT_TRUE(L->IsUp());
}

TEST_F(LinkTest, WentDownOnceAfterLostTimeout)
{
    Start(false);
    L->OnHeard(100);

    EXPECT_THAT(L->Poll(100 + LostMs - 1, true), Not(WentDown()));
    EXPECT_TRUE(L->IsUp());

    EXPECT_THAT(L->Poll(100 + LostMs, true), WentDown());
    EXPECT_FALSE(L->IsUp());

    EXPECT_THAT(L->Poll(100 + LostMs + 1, true), Not(WentDown())) << "reported once only";
    EXPECT_THAT(L->Poll(100 + LostMs * 3, true), Not(WentDown()));
}

TEST_F(LinkTest, HearingThePeerRestartsTheLostTimer)
{
    Start(false);
    L->OnHeard(0);
    L->OnHeard(900);

    EXPECT_THAT(L->Poll(LostMs + 500, true), Not(WentDown()));
    EXPECT_THAT(L->Poll(900 + LostMs, true), WentDown());

    // and it comes back up like the first time
    EXPECT_THAT(L->OnHeard(900 + LostMs + 50), WentUp());
    EXPECT_TRUE(L->IsUp());
}

TEST_F(LinkTest, LostWithNowhereElseToLookStaysOnTheChannel)
{
    Start(true, 1); // channel 1 is home
    L->OnHeard(0);

    // never switched: current, previous and home are all the same place
    const auto Events = L->Poll(LostMs, true);

    EXPECT_THAT(Events, WentDown());
    EXPECT_THAT(Events, Not(Probes()));
    EXPECT_THAT(Events, Not(Settled()));
    EXPECT_EQ(L->GetState(), Link::State::Stable);
    EXPECT_EQ(L->Channel(), 1);

    EXPECT_THAT(L->Poll(LostMs * 4, true), Not(Probes()));
    EXPECT_EQ(L->GetState(), Link::State::Stable);
    EXPECT_EQ(L->Channel(), 1);
}

// ---- agreed channel change --------------------------------------------------

TEST_F(LinkTest, BeginSwitchRejectsBadRequests)
{
    Start(true);

    // nothing may touch the transport before Poll
    EXPECT_CALL(T, SetChannel(_)).Times(0);

    EXPECT_FALSE(L->BeginSwitch(4)) << "no such channel";
    EXPECT_FALSE(L->BeginSwitch(0)) << "already there";
    EXPECT_EQ(L->GetState(), Link::State::Stable);
    EXPECT_FALSE(L->Holding());

    EXPECT_TRUE(L->BeginSwitch(2));
    EXPECT_EQ(L->GetState(), Link::State::SwitchPending);
    EXPECT_TRUE(L->Holding());
    EXPECT_EQ(L->Channel(), 0) << "the retune itself happens in Poll";

    EXPECT_FALSE(L->BeginSwitch(3)) << "one change at a time";
    EXPECT_FALSE(L->BeginSwitch(2));
    Mock::VerifyAndClearExpectations(&T);

    // the first request is the one that is carried out
    EXPECT_CALL(T, SetChannel(2)).Times(1);
    L->Poll(0, true);
    EXPECT_EQ(L->Channel(), 2);
}

TEST_F(LinkTest, PendingSwitchWaitsForTheTransmitter)
{
    Start(true);
    ASSERT_TRUE(L->BeginSwitch(2));

    // still something to send: the transport is left alone however long it takes
    EXPECT_CALL(T, SetChannel(_)).Times(0);
    for (uint32_t Now = 0; Now <= ConfirmMs * 3; Now += 100)
    {
        SCOPED_TRACE(testing::Message() << "poll at " << Now << " ms");
        EXPECT_THAT(L->Poll(Now, false), Not(Probes()));
        EXPECT_EQ(L->GetState(), Link::State::SwitchPending);
        EXPECT_EQ(T.Channel(), 0);
    }
    EXPECT_TRUE(L->Holding());
    Mock::VerifyAndClearExpectations(&T);

    EXPECT_CALL(T, SetChannel(2)).Times(1);
    const auto Events = L->Poll(2000, true);

    EXPECT_THAT(Events, Probes());
    EXPECT_THAT(Events, Not(Settled()));
    EXPECT_EQ(T.Channel(), 2);
    EXPECT_EQ(L->GetState(), Link::State::Confirming);
    EXPECT_FALSE(L->Holding());
    EXPECT_EQ(L->HomeChannel(), 0) << "home does not move with a switch";
}

TEST_F(LinkTest, HearingThePeerConfirmsTheSwitch)
{
    Start(true);
    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 100));

    const auto Events = L->OnHeard(150);

    EXPECT_THAT(Events, Settled());
    EXPECT_THAT(Events, Probes()) << "answer at once, the peer may still be looking";
    EXPECT_THAT(Events, WentUp());
    EXPECT_EQ(L->GetState(), Link::State::Stable);
    EXPECT_EQ(L->Channel(), 2);

    // settled once: later traffic is just traffic
    const auto Later = L->OnHeard(200);
    EXPECT_THAT(Later, Not(Settled()));
    EXPECT_THAT(Later, Not(Probes()));

    // and the confirm timeout no longer applies
    EXPECT_THAT(L->Poll(100 + ConfirmMs + 50, true), Not(Probes()));
    EXPECT_EQ(L->GetState(), Link::State::Stable);
    EXPECT_EQ(L->Channel(), 2);
}

TEST_F(LinkTest, NowOnAnotherChannelTheOldOneIsAValidTarget)
{
    Start(true);
    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 0));
    L->OnHeard(10);

    EXPECT_FALSE(L->BeginSwitch(2)) << "already there";
    EXPECT_TRUE(L->BeginSwitch(0));
}

// ---- searching --------------------------------------------------------------

TEST_F(LinkTest, UnconfirmedSwitchStartsSearchAfterConfirmTimeout)
{
    Start(false);
    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 100));

    L->Poll(100 + ConfirmMs - 1, true);
    EXPECT_EQ(L->GetState(), Link::State::Confirming);

    // The poll that gives up only changes state, the first hop comes with the next one
    const auto GaveUp = L->Poll(100 + ConfirmMs, true);
    EXPECT_EQ(L->GetState(), Link::State::Searching);
    EXPECT_THAT(GaveUp, Not(Probes()));
    EXPECT_THAT(GaveUp, Not(Settled()));
    EXPECT_EQ(L->Channel(), 2);

    // already waited here, so straight on to where we came from
    const auto Hop = L->Poll(100 + ConfirmMs + 1, true);
    EXPECT_THAT(Hop, Probes());
    EXPECT_EQ(L->Channel(), 0);
    EXPECT_EQ(L->GetState(), Link::State::Searching);
}

TEST_F(LinkTest, FollowerSearchAlternatesCurrentAndPreviousWithLongDwell)
{
    Start(false);
    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 0));
    L->Poll(ConfirmMs, true); // -> Searching, candidates {2, 0}

    // follower dwell = the master's longest cycle, 4 candidates
    constexpr uint32_t Dwell = DwellMs * 4;
    uint32_t Now             = ConfirmMs + 1;

    EXPECT_THAT(L->Poll(Now, true), Probes());
    EXPECT_EQ(L->Channel(), 0);

    EXPECT_THAT(L->Poll(Now + DwellMs, true), Not(Probes())) << "a master would have hopped here";
    EXPECT_THAT(L->Poll(Now + Dwell - 1, true), Not(Probes()));
    EXPECT_EQ(L->Channel(), 0);

    Now += Dwell;
    EXPECT_THAT(L->Poll(Now, true), Probes());
    EXPECT_EQ(L->Channel(), 2);

    EXPECT_THAT(L->Poll(Now + Dwell - 1, true), Not(Probes()));
    Now += Dwell;
    EXPECT_THAT(L->Poll(Now, true), Probes());
    EXPECT_EQ(L->Channel(), 0);
}

TEST_F(LinkTest, MasterSearchCyclesCurrentPreviousAndHomeWithShortDwell)
{
    Start(true, 1); // channel 1 is home

    // 1 -> 2 confirmed, then 2 -> 3 never confirmed
    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 0));
    L->OnHeard(10);
    ASSERT_NO_FATAL_FAILURE(SwitchTo(3, 20));
    L->Poll(20 + ConfirmMs, true);
    ASSERT_EQ(L->GetState(), Link::State::Searching);
    ASSERT_EQ(L->Channel(), 3);

    uint32_t Now = 20 + ConfirmMs + 1;

    // current -> previous
    EXPECT_THAT(L->Poll(Now, true), Probes());
    EXPECT_EQ(L->Channel(), 2);

    // master dwell = SearchDwellMs
    EXPECT_THAT(L->Poll(Now + DwellMs - 1, true), Not(Probes()));
    EXPECT_EQ(L->Channel(), 2);

    // previous -> home
    Now += DwellMs;
    EXPECT_THAT(L->Poll(Now, true), Probes());
    EXPECT_EQ(L->Channel(), 1);

    // home -> back to where the search began
    EXPECT_THAT(L->Poll(Now + DwellMs - 1, true), Not(Probes()));
    Now += DwellMs;
    EXPECT_THAT(L->Poll(Now, true), Probes());
    EXPECT_EQ(L->Channel(), 3);

    EXPECT_EQ(L->GetState(), Link::State::Searching);
    EXPECT_EQ(L->HomeChannel(), 1);
}

TEST_F(LinkTest, FollowerDwellDoesNotDependOnTheNumberOfCandidates)
{
    Start(false, 1); // channel 1 is home

    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 0));
    L->OnHeard(10);
    ASSERT_NO_FATAL_FAILURE(SwitchTo(3, 20));
    L->Poll(20 + ConfirmMs, true); // -> Searching, candidates {3, 2, 1}

    constexpr uint32_t Dwell = DwellMs * 4;
    uint32_t Now             = 20 + ConfirmMs + 1;

    EXPECT_THAT(L->Poll(Now, true), Probes());
    EXPECT_EQ(L->Channel(), 2);

    EXPECT_THAT(L->Poll(Now + Dwell - 1, true), Not(Probes()));
    EXPECT_EQ(L->Channel(), 2);

    Now += Dwell;
    EXPECT_THAT(L->Poll(Now, true), Probes());
    EXPECT_EQ(L->Channel(), 1);

    EXPECT_THAT(L->Poll(Now + Dwell - 1, true), Not(Probes()));
    Now += Dwell;
    EXPECT_THAT(L->Poll(Now, true), Probes());
    EXPECT_EQ(L->Channel(), 3);
}

TEST_F(LinkTest, SearchNeverHopsWhileRetuneIsNotAllowed)
{
    Start(true);
    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 0));
    L->Poll(ConfirmMs, false); // giving up on the confirmation does not need the transmitter
    ASSERT_EQ(L->GetState(), Link::State::Searching);

    for (uint32_t Now = ConfirmMs + 1; Now < ConfirmMs + DwellMs * 10; Now += 50)
    {
        const auto Events = L->Poll(Now, false);
        EXPECT_THAT(Events, Not(Probes()));
        EXPECT_EQ(L->Channel(), 2);
    }

    // long overdue, goes as soon as it may
    const uint32_t Released = ConfirmMs + DwellMs * 10;
    EXPECT_THAT(L->Poll(Released, true), Probes());
    EXPECT_EQ(L->Channel(), 0);

    // and the dwell on the new channel counts from the hop, not from when it was due
    EXPECT_THAT(L->Poll(Released + DwellMs - 1, true), Not(Probes()));
    EXPECT_THAT(L->Poll(Released + DwellMs, false), Not(Probes()));
    EXPECT_EQ(L->Channel(), 0);
    EXPECT_THAT(L->Poll(Released + DwellMs, true), Probes());
    EXPECT_EQ(L->Channel(), 2);
}

TEST_F(LinkTest, HearingThePeerWhileSearchingSettlesWhereverWeAre)
{
    Start(true);
    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 0));
    L->Poll(ConfirmMs, true);
    L->Poll(ConfirmMs + 1, true); // hop back to 0
    ASSERT_EQ(L->GetState(), Link::State::Searching);
    ASSERT_EQ(L->Channel(), 0);

    const auto Events = L->OnHeard(ConfirmMs + 50);

    EXPECT_THAT(Events, Settled());
    EXPECT_THAT(Events, Probes());
    EXPECT_THAT(Events, WentUp());
    EXPECT_EQ(L->GetState(), Link::State::Stable);
    EXPECT_EQ(L->Channel(), 0);

    // the search is over, no more hopping
    EXPECT_THAT(L->Poll(ConfirmMs + 50 + DwellMs * 2, true), Not(Probes()));
    EXPECT_EQ(L->Channel(), 0);
    EXPECT_EQ(L->GetState(), Link::State::Stable);
}

TEST_F(LinkTest, HearingThePeerBeforeTheFirstHopSettlesOnTheNewChannel)
{
    Start(false);
    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 0));
    L->Poll(ConfirmMs, true);
    ASSERT_EQ(L->GetState(), Link::State::Searching);

    // the peer was only slow
    const auto Events = L->OnHeard(ConfirmMs);

    EXPECT_THAT(Events, Settled());
    EXPECT_EQ(L->GetState(), Link::State::Stable);
    EXPECT_EQ(L->Channel(), 2);
    EXPECT_THAT(L->Poll(ConfirmMs + 1, true), Not(Probes()));
    EXPECT_EQ(L->Channel(), 2);
}

TEST_F(LinkTest, LosingThePeerAfterASwitchSearchesThePreviousChannel)
{
    Start(true);
    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 0));
    L->OnHeard(10);
    ASSERT_EQ(L->GetState(), Link::State::Stable);

    EXPECT_THAT(L->Poll(10 + LostMs - 1, true), Not(WentDown()));
    EXPECT_EQ(L->GetState(), Link::State::Stable);

    // The peer has been silent here for the whole lost timeout already, so the search
    // starts and makes its first hop in the same poll
    const auto Events = L->Poll(10 + LostMs, true);

    EXPECT_THAT(Events, WentDown());
    EXPECT_THAT(Events, Probes());
    EXPECT_THAT(Events, Not(Settled()));
    EXPECT_EQ(L->GetState(), Link::State::Searching);
    EXPECT_EQ(L->Channel(), 0);

    // then keeps cycling {2, 0}
    EXPECT_THAT(L->Poll(10 + LostMs + DwellMs - 1, true), Not(Probes()));
    EXPECT_THAT(L->Poll(10 + LostMs + DwellMs, true), Probes());
    EXPECT_EQ(L->Channel(), 2);
}

TEST_F(LinkTest, LosingThePeerWhileBusyStartsSearchButWaitsToHop)
{
    Start(true);
    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 0));
    L->OnHeard(10);

    const auto Events = L->Poll(10 + LostMs, false);

    EXPECT_THAT(Events, WentDown());
    EXPECT_THAT(Events, Not(Probes()));
    EXPECT_EQ(L->GetState(), Link::State::Searching);
    EXPECT_EQ(L->Channel(), 2);

    const auto Hop = L->Poll(10 + LostMs + 1, true);
    EXPECT_THAT(Hop, Not(WentDown()));
    EXPECT_THAT(Hop, Probes());
    EXPECT_EQ(L->Channel(), 0);
}

TEST_F(LinkTest, SearchIncludingAddsTheChannelToTheCandidates)
{
    Start(false);

    // never switched, so without the extra channel there would be nowhere to search
    L->SearchIncluding(3, 100);
    ASSERT_EQ(L->GetState(), Link::State::Searching);
    EXPECT_EQ(L->Channel(), 0) << "hops happen in Poll";

    EXPECT_THAT(L->Poll(100, true), Probes());
    EXPECT_EQ(L->Channel(), 3);

    // follower dwell
    EXPECT_THAT(L->Poll(100 + DwellMs * 4 - 1, true), Not(Probes()));
    EXPECT_THAT(L->Poll(100 + DwellMs * 4, true), Probes());
    EXPECT_EQ(L->Channel(), 0);

    EXPECT_THAT(L->Poll(100 + DwellMs * 8, true), Probes());
    EXPECT_EQ(L->Channel(), 3);

    // found it there
    const auto Events = L->OnHeard(100 + DwellMs * 8 + 10);
    EXPECT_THAT(Events, Settled());
    EXPECT_EQ(L->GetState(), Link::State::Stable);
    EXPECT_EQ(L->Channel(), 3);
}

TEST_F(LinkTest, SearchIncludingComesBeforePreviousAndHome)
{
    Start(true, 1); // channel 1 is home
    ASSERT_NO_FATAL_FAILURE(SwitchTo(2, 0));
    L->OnHeard(10);

    // on 2, previous and home are 1, the unconfirmed request was for 3
    L->SearchIncluding(3, 100);
    ASSERT_EQ(L->GetState(), Link::State::Searching);

    EXPECT_THAT(L->Poll(100, true), Probes());
    EXPECT_EQ(L->Channel(), 3);
    EXPECT_THAT(L->Poll(100 + DwellMs, true), Probes());
    EXPECT_EQ(L->Channel(), 1);
    EXPECT_THAT(L->Poll(100 + DwellMs * 2, true), Probes());
    EXPECT_EQ(L->Channel(), 2);
}

TEST_F(LinkTest, SearchIncludingWithNothingNewStaysStable)
{
    Start(true);

    L->SearchIncluding(0, 100); // where we already are
    EXPECT_EQ(L->GetState(), Link::State::Stable);

    L->SearchIncluding(9, 200); // not a channel of this transport
    EXPECT_EQ(L->GetState(), Link::State::Stable);

    EXPECT_THAT(L->Poll(200 + DwellMs * 4, true), Not(Probes()));
    EXPECT_EQ(L->Channel(), 0);
}

// ---- transports without channels --------------------------------------------

TEST_F(LinkTest, SingleChannelTransportHasNoChannelControl)
{
    // a transport with no channel control at all, which must never be asked to retune
    NiceMock<MockTransport> OneChannel {
        TransportCaps {.Kind = TransportKind::Ble, .MaxMessageSize = 100, .RoundTripMs = 50, .ChannelCount = 1}};
    EXPECT_CALL(OneChannel, SetChannel(_)).Times(0);
    L.emplace(OneChannel, LinkTestConfig(true));

    EXPECT_FALSE(L->HasChannels());
    EXPECT_FALSE(L->BeginSwitch(0));
    EXPECT_FALSE(L->BeginSwitch(1));
    EXPECT_EQ(L->GetState(), Link::State::Stable);
    EXPECT_FALSE(L->Holding());

    L->SearchIncluding(1, 0);
    EXPECT_EQ(L->GetState(), Link::State::Stable);
    L->SearchIncluding(0, 0);
    EXPECT_EQ(L->GetState(), Link::State::Stable);

    // reachability still works as on any other transport
    EXPECT_THAT(L->OnHeard(10), WentUp());
    const auto Events = L->Poll(10 + LostMs, true);
    EXPECT_THAT(Events, WentDown());
    EXPECT_THAT(Events, Not(Probes()));
    EXPECT_EQ(L->GetState(), Link::State::Stable);

    EXPECT_THAT(L->Poll(10 + LostMs * 5, true), Not(Probes()));
}

// ---- both ends ----------------------------------------------------------------

// Master and follower start on different channels, each suspecting the other is where it really is
class LinkMeetingTest : public testing::TestWithParam<std::tuple<uint8_t, uint8_t>>
{
};

TEST_P(LinkMeetingTest, MasterAndFollowerOnDifferentChannelsFindEachOther)
{
    constexpr uint32_t StepMs  = 10;
    constexpr uint32_t LimitMs = 5000;
    const auto [MasterStart, FollowerStart] = GetParam();

    sim::SimTransport MasterTransport;
    sim::SimTransport FollowerTransport;
    ASSERT_TRUE(MasterTransport.SetChannel(MasterStart));
    ASSERT_TRUE(FollowerTransport.SetChannel(FollowerStart));

    Link Master {MasterTransport, LinkTestConfig(true)};
    Link Follower {FollowerTransport, LinkTestConfig(false)};

    // each end suspects the other may be where it actually is
    Master.SearchIncluding(FollowerStart, 0);
    Follower.SearchIncluding(MasterStart, 0);
    ASSERT_EQ(Master.GetState(), Link::State::Searching);
    ASSERT_EQ(Follower.GetState(), Link::State::Searching);

    bool Settled       = false;
    uint32_t SettledMs = 0;
    for (uint32_t Now = 0; Now <= LimitMs && !Settled; Now += StepMs)
    {
        Master.Poll(Now, true);
        Follower.Poll(Now, true);

        // on the same channel the probes get through
        if (MasterTransport.Channel() == FollowerTransport.Channel())
        {
            Master.OnHeard(Now);
            Follower.OnHeard(Now);
        }

        if (Master.GetState() == Link::State::Stable && Follower.GetState() == Link::State::Stable)
        {
            Settled   = true;
            SettledMs = Now;
        }
    }

    ASSERT_TRUE(Settled) << "still lost after " << LimitMs << " ms";
    EXPECT_EQ(Master.Channel(), Follower.Channel());
    EXPECT_TRUE(Master.IsUp());
    EXPECT_TRUE(Follower.IsUp());
    // one full follower cycle over its two candidates is plenty
    EXPECT_LE(SettledMs, DwellMs * 2 * 2);

    // and they stay together while they keep hearing each other
    for (uint32_t Now = SettledMs + StepMs; Now <= SettledMs + LostMs * 3; Now += StepMs)
    {
        EXPECT_THAT(Master.Poll(Now, true), Not(Probes()));
        EXPECT_THAT(Follower.Poll(Now, true), Not(Probes()));
        Master.OnHeard(Now);
        Follower.OnHeard(Now);
    }
    EXPECT_EQ(Master.GetState(), Link::State::Stable);
    EXPECT_EQ(Follower.GetState(), Link::State::Stable);
    EXPECT_EQ(Master.Channel(), Follower.Channel());
}

namespace
{
// Every pair of different starting channels
std::vector<std::tuple<uint8_t, uint8_t>> DifferentChannels()
{
    std::vector<std::tuple<uint8_t, uint8_t>> Pairs;
    for (uint8_t Master = 0; Master < 4; ++Master)
    {
        for (uint8_t Follower = 0; Follower < 4; ++Follower)
        {
            if (Master != Follower) { Pairs.emplace_back(Master, Follower); }
        }
    }
    return Pairs;
}

std::string MeetingName(const testing::TestParamInfo<LinkMeetingTest::ParamType>& Info)
{
    return "MasterOn" + std::to_string(std::get<0>(Info.param)) + "FollowerOn" + std::to_string(std::get<1>(Info.param));
}
} // namespace

INSTANTIATE_TEST_SUITE_P(EveryPairOfChannels, LinkMeetingTest, testing::ValuesIn(DifferentChannels()), MeetingName);
