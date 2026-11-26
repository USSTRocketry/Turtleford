/**
 * @file Link.h
 * @brief Per transport link health and channel management.
 *
 * One Link sits on each transport and answers two questions for Radio:
 *   - is the peer currently reachable on this transport?
 *   - which channel should the transport be on?
 *
 * @par Channel changes
 * A change is agreed above this class, by a reliable SwitchChannel message. Each end then calls
 * BeginSwitch(): the link waits until the transport has nothing left to send and no ack is owed,
 * retunes, and waits to hear the peer on the new channel.
 *
 * @par Getting lost
 * The two ends can end up on different channels: the ack of a switch was lost, one end rebooted,
 * the new channel does not work. Both ends then apply the same rule: when the peer is not heard,
 * cycle through the few channels it could be on (current, requested, previous, home), sending a
 * probe on each, and stay on the first one where the peer is heard. The follower dwells on each
 * channel for as long as the master's longest possible cycle, so the two cannot keep missing each
 * other.
 *
 * @par States
 * The channel side is a bricks NodeMachine over the nodes in namespace link. Whether the peer is
 * up runs alongside it, in Link itself.
 * @verbatim
                SwitchTo (another channel)
   +--------+ ----------------------------> +---------------+
   | Stable |                               | SwitchPending | <------------------+
   +--------+ <--------+                    +---------------+                    |
     |    ^            |                            |                            |
     |    |            | Heard                      | Check with CanRetune:      | SwitchTo
     |    |            |                            | retune                     |
     |    |            |                            v                            |
     |    |            |                    +---------------+                    |
     |    |            +------------------- |  Confirming   | -------------------+
     |    |                                 +---------------+                    |
     |    |                                         |                            |
     |    |                                         | ConfirmTimeoutMs over      |
     |    | Heard                                   v                            |
     |    |                                 +---------------+                    |
     |    +-------------------------------- |   Searching   | -------------------+
     |                                      +---------------+
     |              Lost                      ^    |      ^
     +----------------------------------------+    +------+
                                                   Check with CanRetune, dwell over:
                                                   next candidate

   Link::SearchIncluding() goes to Searching from any state. A search with fewer than two
   candidates stays in Stable.
   @endverbatim
 *
 * @verbatim
   From           Event                                  To
   Stable         SwitchTo, another channel              SwitchPending
   Stable         Lost                                   Searching (stays Stable if there is nowhere else to look)
   SwitchPending  Check with CanRetune: retune           Confirming
   Confirming     Heard                                  Stable, ChannelSettled
   Confirming     Check, ConfirmTimeoutMs over           Searching
   Confirming     SwitchTo                               SwitchPending
   Searching      Check with CanRetune, dwell over       Searching, on the next candidate
   Searching      Heard                                  Stable, ChannelSettled
   Searching      SwitchTo                               SwitchPending
   any            Link::SearchIncluding()                Searching
   @endverbatim
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <variant>

#include "Config.h"
#include "DataStructure/StateMachine/NodeMachine.h"
#include "Transport/Transport.h"

namespace ra::turtleford
{
/**
 * @brief Settings of a Link. Radio applies RadioConfig::Link to every transport.
 * @see config::link for the defaults.
 */
struct LinkConfig
{
    /** @brief The peer counts as unreachable after this long without hearing anything from it. */
    uint32_t LostTimeoutMs = config::link::LostTimeoutMs;
    /** @brief After a channel change, how long to wait for the peer before searching for it. */
    uint32_t ConfirmTimeoutMs = config::link::ConfirmTimeoutMs;
    /**
     * @brief Time the master spends on each candidate channel while searching. The follower stays
     * link::MaxCandidates times as long.
     */
    uint32_t SearchDwellMs = config::link::SearchDwellMs;
    /**
     * @brief Exactly one end must be the master. Only the master requests channel changes, and the
     * two ends search at different speeds. Set per device, so it has no entry in Config.h.
     */
    bool ChannelMaster = false;
};

/** @brief What Radio has to act on after a call on a Link. Each flag is set for one call only. */
struct LinkEvents
{
    bool WentUp         = false; ///< The peer became reachable on this transport.
    bool WentDown       = false; ///< Nothing heard from the peer for LinkConfig::LostTimeoutMs.
    bool ChannelSettled = false; ///< A change or a search finished; Link::Channel() is where.
    bool Probe          = false; ///< Send something now so the peer can hear us (just retuned, or just found it).
};

/**
 * @brief The channel state machine of a Link: its events, its context and its nodes Stable,
 * SwitchPending, Confirming and Searching. Internal; use Link.
 */
namespace link
{
/** @brief No channel: Context::Previous and Context::Extra when there is none. */
inline constexpr ChannelNumber NoChannel = std::numeric_limits<uint32_t>::max();
/** @brief Most candidates in a search: current, requested, previous, home. */
inline constexpr uint8_t MaxCandidates = 4;

/** @brief Event: both ends have agreed to move to Channel. */
struct SwitchTo
{
    ChannelNumber Channel; ///< Where to move.
};

/** @brief Event: a valid frame from the peer arrived. */
struct Heard
{
};

/** @brief Event: nothing heard from the peer for LinkConfig::LostTimeoutMs. Sent once, as the link goes down. */
struct Lost
{
    uint32_t NowMs; ///< Current time.
};

/** @brief Event: the periodic poll. */
struct Check
{
    uint32_t NowMs; ///< Current time.
    bool CanRetune; ///< The transport is idle and no ack is owed, so moving loses nothing.
};

/** @brief Shared by the nodes. */
struct Context
{
    ITransport* Transport  = nullptr;   ///< The transport whose channel this is.
    LinkConfig Config {};               ///< The link's settings.
    ChannelNumber Home     = 0;         ///< The transport's channel when the Link was made.
    ChannelNumber Previous = NoChannel; ///< The channel before the last retune done by SwitchPending.
    ChannelNumber Extra    = NoChannel; ///< Requested but unconfirmed channel to include in a search (Link::SearchIncluding()).
    LinkEvents Out {};                  ///< Filled by the nodes during one call on Link.

    /** @brief The channel the transport is on now. */
    ChannelNumber Channel() const { return Transport->Channel(); }
    /** @brief How long a search stays on each channel: LinkConfig::SearchDwellMs on the master, MaxCandidates times that on the follower. */
    uint32_t DwellMs() const;
};

struct Stable;
struct SwitchPending;
struct Confirming;
struct Searching;
/** @brief The nodes of the machine, in the order of Link::State. Stable first: a new Link starts there. */
using Node = std::variant<Stable, SwitchPending, Confirming, Searching>;
/** @brief What a node's On() returns: the node to move to, or nothing to stay. */
using Next = std::optional<Node>;

/**
 * @brief Start a search.
 *
 * The candidates are the current channel, Context::Extra, Context::Previous and Context::Home,
 * in that order, without duplicates and without channels the transport does not have. The first
 * hop happens at the next Check.
 *
 * @return Searching over the candidates, or Stable when there are fewer than two (nowhere else to look).
 */
Node SearchFrom(uint32_t NowMs, const Context& C);

/** @brief On a channel, nothing in progress. Heard needs no handling here. */
struct Stable
{
    /** @brief To SwitchPending, unless already on that channel (then the switch is refused). */
    Next On(const SwitchTo& E, Context& C);
    /** @brief The peer went quiet: search (SearchFrom()). */
    Next On(const Lost& E, Context& C);
};

/** @brief Change agreed, waiting for the transmitter to drain before retuning. A second SwitchTo is refused. */
struct SwitchPending
{
    ChannelNumber Target; ///< Where to retune.

    /** @brief Once CanRetune: remember the channel as Previous, retune, probe, and go to Confirming. */
    Next On(const Check& E, Context& C);
};

/** @brief Retuned, waiting to hear the peer on the new channel. */
struct Confirming
{
    uint32_t SinceMs; ///< When it retuned.

    /** @brief A new change replaces this one: to SwitchPending. */
    Next On(const SwitchTo& E, Context& C);
    /** @brief The peer is here: settled (ChannelSettled, Probe), to Stable. */
    Next On(const Heard& E, Context& C);
    /** @brief After LinkConfig::ConfirmTimeoutMs without the peer: search (SearchFrom()). */
    Next On(const Check& E, Context& C);
};

/** @brief Peer not found, cycling through the candidate channels until it is heard. */
struct Searching
{
    std::array<ChannelNumber, MaxCandidates> Candidates {}; ///< Where to look, in order; the first Count are used.
    uint8_t Count    = 0;                                   ///< Candidates in use, at least 2.
    uint8_t Index    = 0;                                   ///< The candidate it is on.
    uint32_t SinceMs = 0;                                   ///< When it got there.

    /** @brief A change agreed meanwhile replaces the search: to SwitchPending. */
    Next On(const SwitchTo& E, Context& C);
    /** @brief The peer is here: settled (ChannelSettled, Probe), to Stable. */
    Next On(const Heard& E, Context& C);
    /** @brief Once CanRetune and Context::DwellMs() are over: retune to the next candidate and probe. */
    Next On(const Check& E, Context& C);
};
} // namespace link

/**
 * @brief Link health and channel management for one transport.
 *
 * Radio owns one per transport, tells it when the peer is heard (OnHeard()), polls it (Poll()),
 * and acts on the LinkEvents each call returns. Channel changes are started with BeginSwitch()
 * on both ends, or by the master with SearchIncluding() when its request was never confirmed.
 * See the file description for the rules.
 */
class Link
{
public:
    /** @brief The current node of the channel machine. Same order as link::Node. */
    enum class State : uint8_t
    {
        Stable,        ///< On a channel, nothing in progress.
        SwitchPending, ///< Change agreed, waiting for the transmitter to drain before retuning.
        Confirming,    ///< Retuned, waiting to hear the peer on the new channel.
        Searching,     ///< Peer not found, cycling through candidate channels.
    };

    /** @brief What Radio has to act on after a call. */
    using Events = LinkEvents;

public:
    /** @brief An unattached link, to be assigned one made with a transport before use. */
    Link() = default;
    /**
     * @param Transport Must outlive the Link. Its channel now becomes the home channel.
     * @param Config    The link's settings.
     */
    Link(ITransport& Transport, const LinkConfig& Config);

    /** @brief The transport this link manages. */
    ITransport& Transport() const { return *m_Ctx.Transport; }
    /** @brief The current node of the channel machine. */
    State GetState() const { return static_cast<State>(m_Channel.Current().index()); }
    /** @brief The channel the transport is on now. */
    ChannelNumber Channel() const { return m_Ctx.Channel(); }
    /** @brief The transport's channel when the Link was made; a candidate of every search. */
    ChannelNumber HomeChannel() const { return m_Ctx.Home; }

    /** @brief Peer heard within LinkConfig::LostTimeoutMs and the transport can carry traffic. */
    bool IsUp() const { return m_Up && m_Ctx.Transport->IsReady(); }
    /** @brief The transport offers more than one channel. Without that, switches and searches are refused. */
    bool HasChannels() const { return m_Ctx.Transport->Caps().ChannelCount > 1; }
    /** @brief A retune is waiting on the transmitter: queue no new traffic on this transport. Acks may still go. */
    bool Holding() const { return m_Channel.Is<link::SwitchPending>(); }

    /**
     * @brief Both ends have agreed to move to Channel. The retune happens in a later Poll(), once
     * it may retune.
     * @param Channel Where to move.
     * @return False if the transport has no such channel or only one, a retune is already waiting,
     *         or the link is settled on that channel. Allowed while confirming or searching.
     */
    bool BeginSwitch(ChannelNumber Channel);

    /**
     * @brief A requested change was never confirmed (the master's request got no ack), so the peer
     * may or may not have moved. Search for it, with Channel added to the places to look.
     * Replaces whatever the link was doing. Does nothing without HasChannels().
     * @param Channel The channel that was requested.
     * @param NowMs   Current time.
     */
    void SearchIncluding(ChannelNumber Channel, uint32_t NowMs);

    /**
     * @brief A valid frame from the peer arrived on this transport.
     * @param NowMs Current time; restarts the lost timeout.
     * @return WentUp if the peer was not reachable; ChannelSettled and Probe if this ends a
     *         change or a search.
     */
    Events OnHeard(uint32_t NowMs);

    /**
     * @brief The periodic poll: notices the peer going quiet, and makes at most one channel step
     * (a retune that was waiting, the end of a confirm wait, or a search hop).
     * @param NowMs     Current time.
     * @param CanRetune The transport is idle and no ack is owed, so moving loses nothing.
     * @return WentDown when the lost timeout runs out (a stable link then starts searching);
     *         Probe after each retune.
     */
    Events Poll(uint32_t NowMs, bool CanRetune);

private:
    link::Context m_Ctx;                                                     ///< Shared with the nodes.
    bricks::StateMachine::NodeMachine<link::Context, link::Node> m_Channel; ///< The channel machine; starts in Stable.

    bool m_Up              = false; ///< The peer was heard within LinkConfig::LostTimeoutMs.
    uint32_t m_LastHeardMs = 0;     ///< When the peer was last heard.
};

/** @brief Compile-time checks of Link. Internal. */
namespace link::detail
{
/** @brief The Link::State value S names the node N in link::Node. GetState() relies on it. */
template <Link::State S, typename N>
inline constexpr bool Is = std::is_same_v<std::variant_alternative_t<static_cast<size_t>(S), Node>, N>;
static_assert(Is<Link::State::Stable, Stable> && Is<Link::State::SwitchPending, SwitchPending> &&
                  Is<Link::State::Confirming, Confirming> && Is<Link::State::Searching, Searching>,
              "Link::State and link::Node must list the states in the same order");
} // namespace link::detail
} // namespace ra::turtleford
