/**
 * @file TurnTaking.h
 * @brief Who may transmit on a half duplex LoRa link, and when to say "over". Pure logic: no radio,
 * driven by LoraTransport. See LoraTransport.h for the rules on air.
 *
 * Three state machines run side by side, each a bricks NodeMachine:
 *
 *   - Talk, our own side:  `Free -> WindowPending -> Listening -> Free`.
 *     A message we marked "over" has gone out; the listen window starts at the next Advance()
 *     and ends at its deadline, or as soon as the peer says over.
 *   - Guard: Clear or Guarding. After the peer's over, its radio may not be listening yet.
 *   - Peer, what the peer leaves us:  Free, Slot (a short gap in its stream), Holding (it has more
 *     to say), GaveUp (it held us for too long without an over).
 *
 * Guard runs alongside Peer: a peer that goes on talking during the guard, or a stream slot that
 * ends before the guard does, leaves both Holding and Guarding.
 *
 * The peer's over moves Guard and Peer the same way from every state, so the owner does it with
 * Transition() (see NodeMachine.h on emulating superstates); the nodes only handle what differs.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include "DataStructure/StateMachine/NodeMachine.h"
#include "SmallRng.h"

namespace ra::turtleford
{
/**
 * @brief The settings of TurnTaking, all times in ms. It has no defaults of its own: LoraTransport fills every field
 * from LoraTransportConfig (Config.h), so the values there are the only ones.
 */
struct TurnTakingConfig
{
    /** @brief A bulk stream says over once per this long. 0 turns turn taking off. */
    uint32_t ListenEveryMs = 0;
    /** @brief We stay off the air this long (plus up to a quarter more) after a message marked over. */
    uint32_t ListenMs = 0;
    /** @brief After the peer's over we wait this long before we send. */
    uint32_t GuardMs = 0;
    /**
     * @brief A peer not heard for this long has stopped talking. LoraTransport sets it to a whole
     * window plus half a round trip (see its constructor in Src/LoraTransport.cpp).
     */
    uint32_t QuietMs = 0;
    /** @brief Seeds the window jitter; the two ends must differ. */
    uint32_t Seed = 0;
};

/**
 * @brief The nodes and events of the three state machines inside TurnTaking.
 *
 * The sub namespaces talk, guard and peer each hold the nodes of one machine. A node is a struct
 * whose On() overloads handle the events it reacts to; an event it has no On() for is ignored.
 */
namespace turn
{
/** @brief Event: time has moved on. Sent to all three machines by TurnTaking::Advance(). */
struct Tick
{
    uint32_t NowMs; ///< Current time.
};
/** @brief Event: the peer finished a message not marked over: it has more to say. */
struct PeerMore
{
    uint32_t NowMs; ///< Current time.
};
/** @brief Event: the peer finished a message marked over. */
struct PeerOver
{
    uint32_t NowMs; ///< Current time.
};

/** @brief Shared by the nodes of all three machines. */
struct Context
{
    TurnTakingConfig Config;  ///< The settings.
    uint32_t LastHeardMs = 0; ///< Last frame from the peer.
    SmallRng Rng;             ///< Draws the jitter of the listen window.

    /** @param C The settings. Context::Rng is seeded with TurnTakingConfig::Seed. */
    explicit Context(const TurnTakingConfig& C) : Config(C), Rng(C.Seed) {}
};

/** @brief Our own side: are we waiting for the peer to speak after a message marked over? */
namespace talk
{
struct Free;
struct WindowPending;
struct Listening;
/** @brief The nodes of the Talk machine. Free first: TurnTaking starts there. */
using Node  = std::variant<Free, WindowPending, Listening>;
/**
 * @brief What an On() of this machine returns: the node to move to, or nothing to stay.
 * Every On() takes the event (E) and the shared Context (C).
 */
using Next  = std::optional<Node>;

/** @brief Nothing outstanding: we may transmit as far as this machine is concerned. */
struct Free {};
/** @brief A marked message just went out; the window starts at the next Tick. */
struct WindowPending
{
    /** @brief To Listening, with the window ending a jittered Config.ListenMs from now. */
    Next On(const Tick& E, Context& C);
};
/** @brief We said over and stay off the air so the peer can speak. */
struct Listening
{
    uint32_t UntilMs; ///< When the window ends.
    /** @brief To Free once UntilMs is reached. */
    Next On(const Tick& E, Context& C);
    /** @brief The peer has said what it had to say: to Free at once. */
    Next On(const PeerOver& E, Context& C);
};
} // namespace talk

/** @brief The pause after the peer's over, while its radio may not be listening yet. */
namespace guard
{
struct Clear;
struct Guarding;
/** @brief The nodes of the Guard machine. Clear first: TurnTaking starts there. */
using Node  = std::variant<Clear, Guarding>;
/**
 * @brief What an On() of this machine returns: the node to move to, or nothing to stay.
 * Every On() takes the event (E) and the shared Context (C).
 */
using Next  = std::optional<Node>;

/** @brief No guard time running. */
struct Clear {};
/** @brief The peer said over; its radio is surely listening at ReadyMs. */
struct Guarding
{
    uint32_t ReadyMs; ///< When we may send.
    /** @brief To Clear once ReadyMs is reached. */
    Next On(const Tick& E, Context& C);
};
} // namespace guard

/** @brief What the peer leaves us: whether it has more to say and we have to hold our messages. */
namespace peer
{
struct Free;
struct Slot;
struct Holding;
struct GaveUp;
/** @brief The nodes of the Peer machine. Free first: TurnTaking starts there. */
using Node  = std::variant<Free, Slot, Holding, GaveUp>;
/**
 * @brief What an On() of this machine returns: the node to move to, or nothing to stay.
 * Every On() takes the event (E) and the shared Context (C).
 */
using Next  = std::optional<Node>;

/** @brief The peer is not holding us. */
struct Free
{
    /** @brief The peer has more to say: to Holding. */
    Next On(const PeerMore& E, Context& C);
};
/** @brief It said over in the middle of a stream; the stream carries on at ResumeMs. */
struct Slot
{
    uint32_t ResumeMs; ///< When the stream is expected to go on.
    /** @brief To Holding once ResumeMs is reached. */
    Next On(const Tick& E, Context& C);
    /** @brief The stream went on early: to Holding. */
    Next On(const PeerMore& E, Context& C);
};
/** @brief It has more to say: start nothing. More of the same changes nothing. */
struct Holding
{
    uint32_t SinceMs; ///< When we started holding.
    /**
     * @brief To Free when nothing has been heard from the peer for TurnTakingConfig::QuietMs,
     * or to GaveUp after holding for 2 x TurnTakingConfig::ListenEveryMs.
     */
    Next On(const Tick& E, Context& C);
};
/** @brief It held us for too long without an over. Only its next over ends this. */
struct GaveUp {};
} // namespace peer
} // namespace turn

/**
 * @brief Decides when LoraTransport may transmit and which messages are marked over.
 *
 * LoraTransport reports what goes out (MarkOver(), Sent()) and what comes in (Heard(), PeerFinished()),
 * calls Advance() first at every poll, and asks MayTransmit() before it starts a message. All of it
 * does nothing, and MayTransmit() is always true, when Enabled() is false.
 */
class TurnTaking
{
public:
    /** @param Config Copied. */
    explicit TurnTaking(const TurnTakingConfig& Config);

    /** @brief Turn taking is on (ListenEveryMs is not 0). When off, nothing is marked or waited for. */
    bool Enabled() const { return m_Ctx.Config.ListenEveryMs != 0; }

    /**
     * @brief Move the timers to NowMs. Call before anything else at each poll.
     * @param NowMs Current time.
     * @return False if a machine was still moving when it hit its step limit: a cycle among its
     * nodes, which is a bug. For the tests; nothing else to do about it.
     */
    bool Advance(uint32_t NowMs);

    /** @brief A message started now would go on air, as of the last Advance(). True when turn taking is off. */
    bool MayTransmit() const;

    /**
     * @brief Whether the message about to start is marked over: the last one queued, or for bulk,
     * once per ListenEveryMs. Records the over when it is. Never when turn taking is off.
     * @param NowMs      Current time.
     * @param Bulk       The message is one of a stream.
     * @param LastQueued It is the only message in the queue.
     * @return True if the message is to be marked over.
     */
    bool MarkOver(uint32_t NowMs, bool Bulk, bool LastQueued);

    /**
     * @brief A message has finished going out. If it was marked over, the listen window starts at the next Advance().
     * @param Over Whether the message was marked over.
     */
    void Sent(bool Over);

    /**
     * @brief Any frame from the peer arrived.
     * @param NowMs Current time.
     */
    void Heard(uint32_t NowMs);

    /**
     * @brief The peer finished a message, with the marks it carried.
     * @param NowMs Current time.
     * @param Over  The message was marked over.
     * @param Bulk  The message was marked bulk.
     */
    void PeerFinished(uint32_t NowMs, bool Over, bool Bulk);

private:
    /**
     * @brief A NodeMachine over turn::Context.
     * @tparam Node The std::variant of the machine's nodes.
     */
    template <typename Node>
    using Machine = bricks::StateMachine::NodeMachine<turn::Context, Node>;

    turn::Context m_Ctx;                      ///< Shared by the three machines.
    std::optional<uint32_t> m_LastOverMs;     ///< When we last marked a message over; empty before the first.
    Machine<turn::talk::Node> m_Talk;         ///< Our own side.
    Machine<turn::guard::Node> m_Guard;       ///< The pause after the peer's over.
    Machine<turn::peer::Node> m_Peer;         ///< What the peer leaves us.
};
} // namespace ra::turtleford
