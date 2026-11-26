/**
 * @file TurnTaking.cpp
 * @brief The node transitions of the Talk, Guard and Peer machines and the TurnTaking calls that drive them.
 * See Transport/TurnTaking.h.
 */

#include "Transport/TurnTaking.h"

#include "Clock.h"
#include "Config.h"

namespace ra::turtleford
{
namespace turn
{
// ---------------------------------------------------------------------------
// Talk: our own side
// ---------------------------------------------------------------------------

talk::Next talk::WindowPending::On(const Tick& E, Context& C)
{
    // The HAL blocked until the frame was off the air, so the window counts from now. Some more
    // at random, so two talkers do not keep colliding.
    return Listening {.UntilMs = E.NowMs + C.Rng.Jittered(C.Config.ListenMs, config::lora::ListenJitterDivisor)};
}

talk::Next talk::Listening::On(const Tick& E, Context&)
{
    if (TimeReached(E.NowMs, UntilMs)) { return Free {}; }
    return std::nullopt;
}

talk::Next talk::Listening::On(const PeerOver&, Context&)
{
    // It has said what it had to say and listens now: our turn
    return Free {};
}

// ---------------------------------------------------------------------------
// Guard
// ---------------------------------------------------------------------------

guard::Next guard::Guarding::On(const Tick& E, Context&)
{
    if (TimeReached(E.NowMs, ReadyMs)) { return Clear {}; }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Peer
// ---------------------------------------------------------------------------

peer::Next peer::Free::On(const PeerMore& E, Context&)
{
    return Holding {.SinceMs = E.NowMs};
}

peer::Next peer::Slot::On(const Tick& E, Context&)
{
    // A slot in the middle of a stream is short: the stream carries on after it
    if (TimeReached(E.NowMs, ResumeMs)) { return Holding {.SinceMs = E.NowMs}; }
    return std::nullopt;
}

peer::Next peer::Slot::On(const PeerMore& E, Context&)
{
    return Holding {.SinceMs = E.NowMs};
}

peer::Next peer::Holding::On(const Tick& E, Context& C)
{
    // It went quiet without saying over (or its over got lost)
    if (Elapsed(E.NowMs, C.LastHeardMs) >= C.Config.QuietMs) { return Free {}; }

    // It talks on and on and never offers a slot, or we keep missing it: do not wait for ever
    if (Elapsed(E.NowMs, SinceMs) >= 2 * C.Config.ListenEveryMs) { return GaveUp {}; }

    return std::nullopt;
}
} // namespace turn

// ---------------------------------------------------------------------------
// TurnTaking
// ---------------------------------------------------------------------------

TurnTaking::TurnTaking(const TurnTakingConfig& Config) : m_Ctx(Config) {}

bool TurnTaking::Advance(uint32_t NowMs)
{
    if (!Enabled()) { return true; }

    // Settle: a node reached through a deadline checks its own deadlines at the same time, so
    // Slot -> Holding -> Free happens in one call when the peer has been quiet all along
    using bricks::StateMachine::Outcome;
    const turn::Tick E {NowMs};
    const bool TalkSettled  = m_Talk.Settle(E, m_Ctx) != Outcome::Unsettled;
    const bool GuardSettled = m_Guard.Settle(E, m_Ctx) != Outcome::Unsettled;
    const bool PeerSettled  = m_Peer.Settle(E, m_Ctx) != Outcome::Unsettled;
    return TalkSettled && GuardSettled && PeerSettled;
}

bool TurnTaking::MayTransmit() const
{
    if (!Enabled()) { return true; }

    return m_Talk.Is<turn::talk::Free>() && m_Guard.Is<turn::guard::Clear>() && !m_Peer.Is<turn::peer::Holding>();
}

bool TurnTaking::MarkOver(uint32_t NowMs, bool Bulk, bool LastQueued)
{
    if (!Enabled()) { return false; }

    // Say "over" when this is all we had to say, or, in a stream, once in a while
    const bool Over =
        Bulk ? (!m_LastOverMs || Elapsed(NowMs, *m_LastOverMs) >= m_Ctx.Config.ListenEveryMs) : LastQueued;
    if (Over) { m_LastOverMs = NowMs; }
    return Over;
}

void TurnTaking::Sent(bool Over)
{
    if (!Enabled() || !Over) { return; }

    m_Talk.Transition(turn::talk::WindowPending {}, m_Ctx);
}

void TurnTaking::Heard(uint32_t NowMs)
{
    m_Ctx.LastHeardMs = NowMs;
}

void TurnTaking::PeerFinished(uint32_t NowMs, bool Over, bool Bulk)
{
    if (!Enabled()) { return; }

    if (!Over)
    {
        m_Peer.Dispatch(turn::PeerMore {NowMs}, m_Ctx);
        return;
    }

    // It has said what it had to say and listens now: our turn, once its radio is back. The same
    // from every state of Guard and Peer, so done here rather than in each node.
    const TurnTakingConfig& Cfg = m_Ctx.Config;
    m_Talk.Dispatch(turn::PeerOver {NowMs}, m_Ctx);
    m_Guard.Transition(Cfg.GuardMs != 0 ? turn::guard::Node {turn::guard::Guarding {.ReadyMs = NowMs + Cfg.GuardMs}}
                                        : turn::guard::Node {turn::guard::Clear {}},
                       m_Ctx);
    m_Peer.Transition(Bulk ? turn::peer::Node {turn::peer::Slot {.ResumeMs = NowMs + Cfg.ListenMs / config::lora::StreamSlotDivisor}}
                           : turn::peer::Node {turn::peer::Free {}},
                      m_Ctx);
}
} // namespace ra::turtleford
