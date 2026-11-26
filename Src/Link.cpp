/**
 * @file Link.cpp
 * @brief Implements the nodes of the channel state machine and the Link class declared in Link/Link.h.
 */

#include "Link/Link.h"

#include <algorithm>

#include "Clock.h"

namespace ra::turtleford
{
namespace link
{
namespace
{
/**
 * @brief Found the peer here. Answer at once: it may still be looking for us.
 * @param C  The link context. Records the channel as settled and asks for a probe to be sent.
 * @return Stable, the node to move to.
 */
Node Found(Context& C)
{
    C.Extra              = NoChannel;
    C.Out.ChannelSettled = true;
    C.Out.Probe          = true;
    return Stable {};
}
} // namespace

uint32_t Context::DwellMs() const
{
    // The follower stays on each channel for as long as the master's longest cycle (the two ends
    // may hold different candidate lists), so their stays on any one channel overlap. The master's
    // own hops can take a little longer than SearchDwellMs, so the meeting may rely on the
    // follower's probe when it arrives rather than the master's.
    return Config.ChannelMaster ? Config.SearchDwellMs : Config.SearchDwellMs * MaxCandidates;
}

Node SearchFrom(uint32_t NowMs, const Context& C)
{
    Searching S {};
    const auto Add = [&](ChannelNumber Channel)
    {
        if (Channel == NoChannel || Channel >= C.Transport->Caps().ChannelCount) { return; }

        const auto End = S.Candidates.begin() + S.Count;
        if (std::find(S.Candidates.begin(), End, Channel) != End) { return; }

        S.Candidates[S.Count++] = Channel;
    };

    // Everywhere the peer could be, starting with where we are now
    Add(C.Channel());
    Add(C.Extra);
    Add(C.Previous);
    Add(C.Home);

    // Nowhere else to look, keep waiting here
    if (S.Count < 2) { return Stable {}; }

    // We have already waited on the current channel, so move on at the next poll
    S.SinceMs = NowMs - C.DwellMs();
    return S;
}

// ---------------------------------------------------------------------------
// Stable
// ---------------------------------------------------------------------------

Next Stable::On(const SwitchTo& E, Context& C)
{
    // Already there: nothing to do
    if (E.Channel == C.Channel()) { return std::nullopt; }
    return SwitchPending {.Target = E.Channel};
}

Next Stable::On(const Lost& E, Context& C)
{
    return SearchFrom(E.NowMs, C);
}

// ---------------------------------------------------------------------------
// SwitchPending
// ---------------------------------------------------------------------------

Next SwitchPending::On(const Check& E, Context& C)
{
    if (!E.CanRetune) { return std::nullopt; }

    C.Previous = C.Channel();
    C.Transport->SetChannel(Target);
    C.Out.Probe = true;
    return Confirming {.SinceMs = E.NowMs};
}

// ---------------------------------------------------------------------------
// Confirming
// ---------------------------------------------------------------------------

Next Confirming::On(const SwitchTo& E, Context&)
{
    return SwitchPending {.Target = E.Channel};
}

Next Confirming::On(const Heard&, Context& C)
{
    return Found(C);
}

Next Confirming::On(const Check& E, Context& C)
{
    if (Elapsed(E.NowMs, SinceMs) >= C.Config.ConfirmTimeoutMs) { return SearchFrom(E.NowMs, C); }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------------

Next Searching::On(const SwitchTo& E, Context&)
{
    return SwitchPending {.Target = E.Channel};
}

Next Searching::On(const Heard&, Context& C)
{
    return Found(C);
}

Next Searching::On(const Check& E, Context& C)
{
    if (!E.CanRetune || Elapsed(E.NowMs, SinceMs) < C.DwellMs()) { return std::nullopt; }

    // On to the next candidate; still searching
    Index   = static_cast<uint8_t>((Index + 1) % Count);
    SinceMs = E.NowMs;
    C.Transport->SetChannel(Candidates[Index]);
    C.Out.Probe = true;
    return std::nullopt;
}
} // namespace link

// ---------------------------------------------------------------------------
// Link
// ---------------------------------------------------------------------------

Link::Link(ITransport& Transport, const LinkConfig& Config) :
    m_Ctx {.Transport = &Transport, .Config = Config, .Home = Transport.Channel()}
{
}

bool Link::BeginSwitch(ChannelNumber Channel)
{
    if (!HasChannels() || Channel >= m_Ctx.Transport->Caps().ChannelCount) { return false; }

    // Refused (no transition) while a retune is already waiting, or when settled on that channel
    return m_Channel.Dispatch(link::SwitchTo {Channel}, m_Ctx) == bricks::StateMachine::Outcome::Moved;
}

void Link::SearchIncluding(ChannelNumber Channel, uint32_t NowMs)
{
    if (!HasChannels()) { return; }

    // From any state: a search replaces any change that was still waiting to happen
    m_Ctx.Extra = Channel;
    m_Channel.Transition(link::SearchFrom(NowMs, m_Ctx), m_Ctx);
}

Link::Events Link::OnHeard(uint32_t NowMs)
{
    m_Ctx.Out = {};

    m_LastHeardMs = NowMs;
    if (!m_Up)
    {
        m_Up             = true;
        m_Ctx.Out.WentUp = true;
    }

    m_Channel.Dispatch(link::Heard {}, m_Ctx);
    return m_Ctx.Out;
}

Link::Events Link::Poll(uint32_t NowMs, bool CanRetune)
{
    m_Ctx.Out = {};

    if (m_Up && Elapsed(NowMs, m_LastHeardMs) >= m_Ctx.Config.LostTimeoutMs)
    {
        m_Up               = false;
        m_Ctx.Out.WentDown = true;
        // Only a stable link starts searching; one already moving carries on
        m_Channel.Dispatch(link::Lost {NowMs}, m_Ctx);
    }

    // One step: a search started just now makes its first hop here, one started by a confirm
    // timeout at the next poll
    m_Channel.Dispatch(link::Check {NowMs, CanRetune}, m_Ctx);
    return m_Ctx.Out;
}
} // namespace ra::turtleford
