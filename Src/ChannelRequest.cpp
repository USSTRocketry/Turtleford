/**
 * @file ChannelRequest.cpp
 * @brief Implements the nodes of the request state machine and the ChannelRequest class declared in
 * Link/ChannelRequest.h.
 */

#include "Link/ChannelRequest.h"

namespace ra::turtleford
{
namespace request
{
Next OnItsWay::On(const Answer&, Context&)
{
    return Answered {};
}

Next RetryOwed::On(const Accepted& E, Context&)
{
    return OnItsWay {.Id = E.Id};
}

Next Answered::On(const Settled& E, Context& C)
{
    if (E.Here != C.Wanted && C.RetriesLeft > 0)
    {
        // Back where we started: the peer did not follow, or we lost it on the way. Ask again.
        --C.RetriesLeft;
        return RetryOwed {};
    }
    return Idle {};
}
} // namespace request

void ChannelRequest::Begin(ChannelNumber Channel, uint8_t Retries, Ticket Id)
{
    m_Ctx = request::Context {.Wanted = Channel, .RetriesLeft = Retries};
    m_Machine.Transition(request::OnItsWay {.Id = Id}, m_Ctx);
}

void ChannelRequest::Submitted(Ticket Id)
{
    m_Machine.Dispatch(request::Accepted {Id}, m_Ctx);
}

void ChannelRequest::Answered()
{
    m_Machine.Dispatch(request::Answer {}, m_Ctx);
}

ChannelRequest::Next ChannelRequest::OnSettled(ChannelNumber Here)
{
    // Only an answered request takes a settle as its outcome
    if (m_Machine.Dispatch(request::Settled {Here}, m_Ctx) == bricks::StateMachine::Outcome::Stayed)
    {
        // Nothing to finish, or the request is still on its way or its retry is waiting for room
        return Active() ? Next::Wait : Next::Report;
    }
    return SubmitOwed() ? Next::Retry : Next::Report;
}
} // namespace ra::turtleford
