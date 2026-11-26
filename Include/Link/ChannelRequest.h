/**
 * @file ChannelRequest.h
 * @brief The channel master's side of a channel change on one transport: the channel asked for,
 * the request on its way, and the retries left.
 *
 * Radio submits the SwitchChannel request through the ARQ, tells this class what became of it
 * (Answered()) and where the link settled (OnSettled()), and does what OnSettled() returns: report
 * the channel to the application, wait, or submit the request again.
 *
 * @par States
 * A bricks NodeMachine over the nodes in namespace request:
 * @verbatim
       Idle --Begin--> OnItsWay --Answered--> Answered --settled on Wanted, or no retry left--> Idle
                          ^                      |
                          |                      +--settled elsewhere, a retry left--> RetryOwed
                          +-------------------------------Submitted------------------------+
   @endverbatim
 * Every question Radio asks is answered from the current node, so the nodes write no outputs.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include "DataStructure/StateMachine/NodeMachine.h"
#include "Transport/Transport.h"

namespace ra::turtleford
{
/**
 * @brief The state machine behind ChannelRequest: its events, its context and its nodes.
 * Internal; use ChannelRequest.
 */
namespace request
{
/** @brief The ARQ ticket of the request on its way (Radio's Ticket). */
using Ticket = uint32_t;
/** @brief No ticket: never the ticket of a request on its way. */
inline constexpr Ticket NoTicket = 0;

/** @brief Event: the ARQ took the owed retry. */
struct Accepted
{
    Ticket Id; ///< The retry's ticket.
};

/** @brief Event: the request on its way was acked, or given up on. */
struct Answer
{
};

/** @brief Event: the link settled on a channel (a change finished, or a search found the peer). */
struct Settled
{
    ChannelNumber Here; ///< Where it settled.
};

/** @brief Shared by the nodes: what the request is after. */
struct Context
{
    ChannelNumber Wanted = 0; ///< The channel asked for.
    uint8_t RetriesLeft  = 0; ///< Further attempts allowed if it does not end on Wanted.
};

struct Idle;
struct OnItsWay;
struct RetryOwed;
struct Answered;
/** @brief The nodes of the machine. Idle first: a new ChannelRequest starts there. */
using Node = std::variant<Idle, OnItsWay, RetryOwed, Answered>;
/** @brief What a node's On() returns: the node to move to, or nothing to stay. */
using Next = std::optional<Node>;

/** @brief No request, or the last one is finished. Handles no event. */
struct Idle
{
};

/** @brief Submitted to the ARQ, no answer yet. A settle now is not the request's outcome. */
struct OnItsWay
{
    Ticket Id; ///< Its ARQ ticket.

    /** @brief Acked or given up on: wait for the link to settle. */
    Next On(const Answer& E, Context& C);
};

/** @brief A retry is due and the ARQ has not taken it yet (its queue was full). */
struct RetryOwed
{
    /** @brief The ARQ took it: on its way again. */
    Next On(const Accepted& E, Context& C);
};

/** @brief The peer answered (or was given up on); the next settle is the outcome. */
struct Answered
{
    /** @brief Settled elsewhere with a retry left: RetryOwed, using up one retry. Otherwise Idle. */
    Next On(const Settled& E, Context& C);
};
} // namespace request

/**
 * @brief The master's channel change request on one transport, with its retries.
 *
 * One per transport, owned by Radio. A change can end on another channel than the one asked for:
 * the ack is lost and both ends fall back to the old channel, or the new channel does not work.
 * This class decides whether that settle is the final outcome (report it to the application) or
 * calls for another attempt, up to the number of retries given to Begin().
 *
 * Settles that happen while no request is active (the peer found again after an outage, any
 * settle on the follower end) are reported straight away.
 */
class ChannelRequest
{
public:
    /** @copydoc request::Ticket */
    using Ticket = request::Ticket;
    /** @copydoc request::NoTicket */
    static constexpr Ticket NoTicket = request::NoTicket;

    /** @brief What Radio does after OnSettled(). */
    enum class Next : uint8_t
    {
        Report, ///< Tell the application where the link is; the request, if there was one, is finished.
        Wait,   ///< Not the request's outcome yet: it is still on its way, or a retry waits for room.
        Retry,  ///< It did not end on the wanted channel and a retry is left: submit it again.
    };

    /** @brief A request, its retries included, is unfinished. Radio::RequestChannel() refuses a new one meanwhile. */
    bool Active() const { return !m_Machine.Is<request::Idle>(); }

    /** @brief The channel the request is after. Meaningful while Active(). */
    ChannelNumber Wanted() const { return m_Ctx.Wanted; }

    /**
     * @brief Whether a ticket is the request on its way, so its ack or failure belongs here.
     * @return True if Id is that ticket. Never true for NoTicket.
     */
    bool Owns(Ticket Id) const
    {
        const auto* Way = m_Machine.Get<request::OnItsWay>();
        return Id != NoTicket && Way != nullptr && Way->Id == Id;
    }

    /** @brief A retry is due and the ARQ has not accepted it yet (its queue was full). */
    bool SubmitOwed() const { return m_Machine.Is<request::RetryOwed>(); }

    /**
     * @brief The first request was accepted by the ARQ. Starts over whatever came before.
     * @param Channel The channel asked for.
     * @param Retries Further attempts allowed if it does not end on Channel.
     * @param Id      The request's ARQ ticket.
     */
    void Begin(ChannelNumber Channel, uint8_t Retries, Ticket Id);

    /**
     * @brief The owed retry (SubmitOwed()) was accepted by the ARQ.
     * @param Id The retry's ARQ ticket.
     */
    void Submitted(Ticket Id);

    /** @brief The request on its way was acked or given up on; it is no longer on its way. */
    void Answered();

    /**
     * @brief The link settled on a channel: a change finished, or a search found the peer.
     * @param Here Where it settled.
     * @return What Radio has to do. Anything but Wait answers this settle. A Retry leaves a retry
     *         owed, which Radio submits, and reports through Submitted() once the ARQ has taken it.
     */
    Next OnSettled(ChannelNumber Here);

private:
    request::Context m_Ctx;                                                        ///< What the request is after.
    bricks::StateMachine::NodeMachine<request::Context, request::Node> m_Machine; ///< Starts in Idle.
};
} // namespace ra::turtleford
