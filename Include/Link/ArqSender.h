/**
 * @file ArqSender.h
 * @brief The sending half of the sliding window: numbering, retransmission, giving up.
 * See Arq.h for how the two halves work together.
 *
 * The messages are held in a bricks::CircularBuffer: a FIFO, since they are numbered, finished
 * with and given up on oldest first, with indexed access, since the ones in the window are
 * visited by position.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "Clock.h"
#include "Config.h"
#include "DataStructure/Buffer/CircularBuffer.h"
#include "Link/Arq.h"

namespace ra::turtleford
{
/**
 * @brief Settings of an ArqSender. Radio takes them as RadioConfig::Sender.
 * @see config::arq for the defaults.
 */
struct ArqSenderConfig
{
    /** @brief Most messages on air and unacknowledged at once. Clamped to 1 .. Capacity. */
    uint16_t Window = config::arq::SenderWindow;
    /**
     * @brief Most messages held in total: in the window plus waiting behind it. Clamped to
     * 1 .. the sender's MaxCapacity.
     */
    uint16_t Capacity = config::arq::SenderCapacity;
    /** @brief Transmissions of one message before it is given up on. At least 1. */
    uint8_t MaxAttempts = config::arq::MaxAttempts;
    /** @brief The retransmit timeout doubles per attempt, up to 2^MaxBackoffShift times the base. */
    uint8_t MaxBackoffShift = config::arq::MaxBackoffShift;
};

/**
 * @brief The sending half of the sliding window.
 *
 * Holds every message from Submit() until the peer has delivered it (OnAck()) or it is given up
 * on (Abandon()). Numbers them, says which one is due on air (NextDue()), and times their
 * retransmission, doubling the timeout per attempt. Messages are finished with oldest first, in
 * the order they were submitted.
 *
 * @tparam T            The payload of a message. Copied in by Submit().
 * @tparam MaxCapacity  Room for this many messages, allocated with the sender.
 *                      ArqSenderConfig::Capacity can only lower it.
 */
template <typename T, uint16_t MaxCapacity = config::arq::SenderCapacity>
class ArqSender
{
public:
    /** @brief Identifies a submitted message to the caller. Never InvalidTicket for a real one. */
    using Ticket = uint32_t;
    /** @brief The ticket Submit() returns when it refuses a message. */
    static constexpr Ticket InvalidTicket = 0;

    /** @brief A message that is due on air, as NextDue() returns it. */
    struct Frame
    {
        Ticket Id;          ///< The ticket Submit() gave the message.
        const T* Payload;   ///< Valid until the message is finished with or the sender is changed.
        SeqNum Seq;         ///< Its sequence number, to put on the frame.
        uint8_t Attempt;    ///< 0 for the first transmission, 1 for the first retransmission, ...
    };

public:
    /** @param Config Clamped as each field describes. */
    explicit ArqSender(const ArqSenderConfig& Config = {}) : m_Config(Config)
    {
        m_Config.Capacity    = std::clamp<uint16_t>(m_Config.Capacity, 1, MaxCapacity);
        m_Config.Window      = std::clamp<uint16_t>(m_Config.Window, 1, m_Config.Capacity);
        m_Config.MaxAttempts = std::max<uint8_t>(m_Config.MaxAttempts, 1);
    }

    /** @brief No room for another message: Submit() would refuse it. */
    bool Full() const { return m_Slots.Size() >= m_Config.Capacity; }
    /** @brief No message is held. */
    bool Empty() const { return m_Slots.Empty(); }
    /** @brief Messages held: on air, waiting for an ack, or waiting behind the window. */
    size_t Pending() const { return m_Slots.Size(); }
    /** @brief Most messages held at once, after clamping. */
    size_t Capacity() const { return m_Config.Capacity; }
    /** @brief Messages Submit() would still accept. */
    size_t Free() const { return m_Config.Capacity - m_Slots.Size(); }

    /**
     * @brief Hand over a message for reliable delivery.
     * @param Payload Copied into the sender.
     * @return A ticket identifying the message, or InvalidTicket when there is no room (Full()).
     */
    Ticket Submit(const T& Payload)
    {
        if (Full()) { return InvalidTicket; }

        const Ticket Id = NextTicket();
        m_Slots.Queue(Slot {.Payload = Payload, .Id = Id});
        return Id;
    }

    /**
     * @brief The frame that should go on air now: a first transmission or a timed out one.
     *
     * Only messages inside the window are considered, oldest first. One the peer reported holding
     * (selective ack) is not sent again while an older one is still unfinished. Once it is the
     * oldest (the one before it was given up on) it is treated like any other: only the peer can
     * finish it, and a silent peer must not leave it, and everything behind it, waiting for ever.
     *
     * @param NowMs Current time.
     * @return The frame to send, or nothing when no frame is due. Record what was done with it
     *         through MarkSent() or MarkQueued().
     */
    std::optional<Frame> NextDue(uint32_t NowMs) const
    {
        for (uint32_t i = 0; i < InWindow(); ++i)
        {
            const Slot& S = m_Slots[i];
            if ((S.Acked && i > 0) || S.Attempts >= m_Config.MaxAttempts) { continue; }
            if (S.At == Phase::Unsent || TimeReached(NowMs, S.DeadlineMs))
            {
                return Frame {.Id = S.Id, .Payload = &S.Payload, .Seq = SeqAt(i), .Attempt = S.Attempts};
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Record that the frame from NextDue() has left: its retransmit timeout starts now.
     * @param Seq   Frame::Seq of that frame. Ignored if it is not in the window.
     * @param NowMs Current time.
     * @param RtoMs Base retransmit timeout of the transport it went out on, before the backoff.
     */
    void MarkSent(SeqNum Seq, uint32_t NowMs, uint32_t RtoMs)
    {
        const auto Index = static_cast<SeqNum>(Seq - m_Base);
        if (Index >= InWindow()) { return; }

        Slot& S      = m_Slots[Index];
        S.DeadlineMs = Deadline(NowMs, RtoMs, S.Attempts);
        S.At         = Phase::OnAir;
        ++S.Attempts;
    }

    /**
     * @brief Record that the frame from NextDue() was accepted by a transport but is still
     * waiting in its queue.
     *
     * The retransmit timeout does not run yet: StartTimers() starts it once the frame has left.
     * Until then only the stall limit applies. The attempt is counted now.
     *
     * @param Seq     Frame::Seq of that frame. Ignored if it is not in the window.
     * @param NowMs   Current time.
     * @param StallMs If the frame has still not left after this long it is due again.
     * @param Via     Opaque to this class; handed back by StartTimers() (Radio: the transport's index).
     * @param Mark    Opaque to this class; handed back by StartTimers() (Radio: the transport's
     *                ITransport::TxCompleted() count at which this frame has left).
     */
    void MarkQueued(SeqNum Seq, uint32_t NowMs, uint32_t StallMs, uint8_t Via, uint32_t Mark)
    {
        const auto Index = static_cast<SeqNum>(Seq - m_Base);
        if (Index >= InWindow()) { return; }

        Slot& S      = m_Slots[Index];
        S.DeadlineMs = NowMs + StallMs;
        S.At         = Phase::Queued;
        S.Via        = Via;
        S.Mark       = Mark;
        ++S.Attempts;
    }

    /**
     * @brief Start the retransmit timeout of every queued frame that has now left its transport.
     * @param NowMs Current time; the timeout counts from here.
     * @param Left  Called as `uint32_t Left(uint8_t Via, uint32_t Mark)` for each frame marked
     *              with MarkQueued(). Returns the base retransmit timeout of the transport if
     *              that frame has left it, 0 if it is still waiting.
     */
    template <typename F>
    void StartTimers(uint32_t NowMs, F&& Left)
    {
        for (uint32_t i = 0; i < InWindow(); ++i)
        {
            Slot& S = m_Slots[i];
            if (S.At != Phase::Queued) { continue; }

            const uint32_t RtoMs = Left(S.Via, S.Mark);
            if (RtoMs == 0) { continue; }

            // MarkQueued() counted this attempt already, so the ones before it are one fewer
            S.DeadlineMs = Deadline(NowMs, RtoMs, static_cast<uint8_t>(S.Attempts - 1));
            S.At         = Phase::OnAir;
        }
    }

    /**
     * @brief Give up on the oldest message once its last attempt has timed out, and on each one
     * after it that has too.
     *
     * Only the oldest is given up on, so everything before SkipNotice() is finished with and one
     * number tells the receiver what to stop waiting for. A later message that runs out of
     * attempts first waits until it is the oldest. One the peer reported holding is resent once it
     * is the oldest (see NextDue()), and given up on like any other if the peer never answers.
     *
     * @param NowMs  Current time.
     * @param Failed Called as `Failed(Ticket)` for each message given up on, oldest first. May
     *               call Submit().
     */
    template <typename F>
    void Abandon(uint32_t NowMs, F&& Failed)
    {
        while (!Empty())
        {
            const Slot& S = m_Slots[0];
            if (S.Attempts < m_Config.MaxAttempts || !TimeReached(NowMs, S.DeadlineMs)) { return; }

            const Ticket Id = S.Id;
            PopFront();
            Failed(Id);
        }
    }

    /**
     * @brief What to tell the receiver after giving up on messages.
     *
     * Send it to the receiver (ArqReceiver::OnSkip()) with every frame until an ack shows the
     * receiver has moved past it.
     *
     * @return The oldest seq still held, while the receiver may still be waiting for one before it
     *         that was given up on. Nothing when the receiver is not waiting for anything given up on.
     */
    std::optional<SeqNum> SkipNotice() const
    {
        if (m_Base == m_PeerNext) { return std::nullopt; }
        return m_Base;
    }

    /**
     * @brief Process an acknowledgement from the peer.
     *
     * Messages covered by the cumulative ack are finished with. Messages the selective ack reports
     * as held are not sent again for now. Unacknowledged messages on air before the highest one
     * reported held were lost, so they are made due at once instead of after their timeout (once
     * per message). An ack that covers a seq never transmitted is stale or corrupt and is ignored.
     *
     * @param Next      Cumulative ack: every seq before this one has been delivered.
     * @param Sack      Selective ack: bit N set means seq (Next + 1 + N) is held behind a gap.
     * @param NowMs     Current time.
     * @param Delivered Called as `Delivered(Ticket)` for each message the peer has delivered,
     *                  oldest first.
     */
    template <typename F>
    void OnAck(SeqNum Next, uint16_t Sack, uint32_t NowMs, F&& Delivered)
    {
        const auto Advance = static_cast<SeqNum>(Next - m_Base);

        // An ack can only cover what was actually transmitted, anything else is stale or corrupt
        if (Advance > InWindow()) { return; }
        for (uint32_t i = 0; i < Advance; ++i)
        {
            if (m_Slots[i].At == Phase::Unsent) { return; }
        }
        m_PeerNext = Next;

        for (uint32_t i = 0; i < Advance; ++i)
        {
            const Ticket Id = m_Slots[0].Id;
            PopFront();
            Delivered(Id);
        }

        // Frames the peer holds out of order need no retransmission
        uint32_t Highest = 0;
        for (uint32_t Bit = 0; Bit < 16; ++Bit)
        {
            const uint32_t Index = Bit + 1;
            if (!(Sack & (1u << Bit)) || Index >= InWindow()) { continue; }

            Slot& S = m_Slots[Index];
            if (S.At == Phase::Unsent) { continue; }

            S.Acked = true;
            Highest = Index;
        }

        // The peer got something later, so everything unacked before it was lost: resend it
        // straight away instead of waiting for the timeout. Once per message.
        for (uint32_t i = 0; i < Highest; ++i)
        {
            Slot& S = m_Slots[i];
            // Only one on air can have been lost: one unsent or still waiting in a transport queue
            // has not been sent. One with no attempt left is decided by its last timeout, as for any other.
            if (S.Acked || S.At != Phase::OnAir || S.FastResent || S.Attempts >= m_Config.MaxAttempts) { continue; }

            S.FastResent = true;
            S.DeadlineMs = NowMs;
        }
    }

    /**
     * @brief The peer lost its state (it rebooted). Keep every unfinished message, number them
     * again from 0 and send them all afresh. Their tickets do not change.
     */
    void Restart()
    {
        m_Base     = 0;
        m_PeerNext = 0;
        for (uint32_t i = 0; i < m_Slots.Size(); ++i)
        {
            Slot& S      = m_Slots[i];
            S.At         = Phase::Unsent;
            S.Attempts   = 0;
            S.Acked      = false;
            S.FastResent = false;
        }
    }

private:
    /** @brief Where the latest attempt of a message is. Slot::Attempts is 0 exactly while Unsent. */
    enum class Phase : uint8_t
    {
        Unsent, ///< No attempt yet.
        Queued, ///< Accepted by a transport, not yet on air: only the stall limit runs (Slot::Via, Slot::Mark).
        OnAir,  ///< Left the transport: the retransmit timeout runs.
    };

    /**
     * @brief One held message. Acked and FastResent are flags beside the phase: a message can be
     * acked while a later attempt of it waits in a transport queue.
     */
    struct Slot
    {
        T Payload {};                        ///< Copy of what was submitted.
        Ticket Id           = InvalidTicket; ///< Returned by Submit().
        uint32_t DeadlineMs = 0;             ///< When the current attempt times out (or stalls, while Queued).
        uint8_t Attempts    = 0;             ///< Transmissions so far, the current one included.
        Phase At            = Phase::Unsent; ///< Where the latest attempt is.
        bool Acked          = false;         ///< The peer holds it, waiting for an earlier message.
        bool FastResent     = false;         ///< Already made due early once by OnAck().
        uint8_t Via         = 0;             ///< Which transport, as given to MarkQueued().
        uint32_t Mark       = 0;             ///< Its position in that transport's output, as given to MarkQueued().
    };

    /** @brief Messages inside the window: the first this many held, at most Window. */
    uint32_t InWindow() const { return std::min<uint32_t>(m_Slots.Size(), m_Config.Window); }
    /** @brief The seq of the message at Index in m_Slots. */
    SeqNum SeqAt(uint32_t Index) const { return static_cast<SeqNum>(m_Base + Index); }

    /**
     * @brief When an attempt made at NowMs times out.
     * @param Retries Attempts made before this one: MarkSent() has not counted this one yet,
     *                StartTimers() has (so it passes one less).
     */
    uint32_t Deadline(uint32_t NowMs, uint32_t RtoMs, uint8_t Retries) const
    {
        return NowMs + (RtoMs << std::min<uint8_t>(Retries, m_Config.MaxBackoffShift));
    }

    /** @brief Finish with the oldest message and move the window on by one. */
    void PopFront()
    {
        m_Slots.Discard();
        ++m_Base;
    }

    /** @brief The next ticket, skipping InvalidTicket when the count wraps. */
    Ticket NextTicket()
    {
        if (++m_LastTicket == InvalidTicket) { ++m_LastTicket; }
        return m_LastTicket;
    }

private:
    ArqSenderConfig m_Config;                          ///< After clamping.
    bricks::CircularBuffer<Slot, MaxCapacity> m_Slots; ///< Oldest first; index 0 has seq m_Base.
    SeqNum m_Base       = 0;             ///< Seq of the oldest message still held.
    SeqNum m_PeerNext   = 0;             ///< The receiver's next expected seq, as its last accepted ack said.
    Ticket m_LastTicket = InvalidTicket; ///< The ticket given out last.
};
} // namespace ra::turtleford
