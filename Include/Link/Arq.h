/**
 * @file Arq.h
 * @brief Selective repeat sliding window (ARQ), split into a sending half (ArqSender) and a
 * receiving half (ArqReceiver), and the sequence number type they share.
 *
 * Both halves are pure logic: no I/O, no clock and no knowledge of what a payload is. The caller
 * passes in the current time and what arrived, and asks what should go on air next, so both can
 * be tested without a radio. Radio owns one of each, shared by every transport.
 *
 * @par How the two halves talk
 * @verbatim
   ArqSender                                   ArqReceiver
     Submit(payload)                             OnFrame(seq, payload)  -> delivers in order
     NextDue(now)  -> frame to (re)send          AckNext() / AckSack()  -> sent back to the sender
     MarkSent(seq, now, rto)
       or MarkQueued(...) + StartTimers(...)
       when the transport queues the frame
     OnAck(next, sack)  <-------------------------------'
     Abandon(now)  -> gives up on the oldest     OnSkip(oldest)  -> stops waiting for what the
     SkipNotice()  -> sent to the receiver ----->                   sender gave up on
   @endverbatim
 *
 * @par Ordering and loss
 * Sequence numbers are 16 bit and wrap. The receiver accepts frames out of order anywhere inside
 * its window and holds them until the gap before them is filled, so messages reach the application
 * in the order they were submitted. A message the sender gives up on is skipped: the ones after it
 * still arrive, in order, and none is delivered twice.
 */

#pragma once

#include <cstdint>

namespace ra::turtleford
{
/**
 * @brief Sequence number of a reliable message.
 *
 * 16 bit and wraps, so two numbers are compared by their difference (cast back to SeqNum),
 * never with <.
 */
using SeqNum = uint16_t;
} // namespace ra::turtleford
