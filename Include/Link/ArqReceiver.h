/**
 * @file ArqReceiver.h
 * @brief The receiving half of the sliding window: holding out-of-order frames, in-order
 * delivery, duplicates, acks. See Arq.h for how the two halves work together.
 *
 * The frames are held in a bricks::RingWindow: a fixed window whose slots are filled out of
 * order, at their distance from the next expected seq, which a FIFO cannot hold. Sliding it never
 * clears a slot, so what a slide passed over can still be read to hand it over.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "Config.h"
#include "DataStructure/Buffer/RingWindow.h"
#include "Link/Arq.h"

namespace ra::turtleford
{
/**
 * @brief Settings of an ArqReceiver. Radio takes them as RadioConfig::Receiver.
 * @see config::arq for the defaults.
 */
struct ArqReceiverConfig
{
    /**
     * @brief How far ahead of the next expected message a frame is accepted and held.
     * Clamped to 1 .. ArqReceiver::MaxWindow.
     */
    uint16_t Window = config::arq::ReceiverWindow;
};

/**
 * @brief The receiving half of the sliding window.
 *
 * Takes reliable frames in any order (OnFrame()), holds the ones that arrive ahead of a gap, and
 * hands messages over in sequence order, each exactly once. Says what to acknowledge (AckNext(),
 * AckSack()) and whether an ack is owed (AckPending()). Stops waiting for messages the sender
 * gave up on when told (OnSkip()).
 *
 * @tparam T The payload of a message. Copied in by OnFrame() while it waits behind a gap.
 */
template <typename T>
class ArqReceiver
{
public:
    /** @brief What OnFrame() did with a frame. */
    enum class Result
    {
        Delivered,   ///< Handed over, possibly with frames that were waiting on it.
        Buffered,    ///< Ahead of a gap; held until the gap is filled.
        Duplicate,   ///< Already delivered or already held; the ack is repeated.
        OutOfWindow, ///< Too far ahead to hold; dropped. The sender resends it later.
    };

    /**
     * @brief Largest window. The selective ack bitmap is 16 bits and its bit 0 is the seq after
     * the next expected one, so 1 + 16 seqs can be described. Also the bit count of Slide()'s mask.
     */
    static constexpr uint16_t MaxWindow = 17;

public:
    /** @param Config ArqReceiverConfig::Window is clamped to 1 .. MaxWindow. */
    explicit ArqReceiver(const ArqReceiverConfig& Config = {}) : m_Window(std::clamp<uint16_t>(Config.Window, 1, MaxWindow))
    {
    }

    /**
     * @brief Process one reliable frame.
     *
     * Every frame, a duplicate or one out of the window included, makes an ack owed
     * (AckPending()), so an ack that was lost is repaired by the retransmission.
     *
     * @param Seq     The frame's sequence number.
     * @param Payload Its message. Copied if the frame is held.
     * @param Deliver Called as `Deliver(const T&)` for each message that is now in order: this one,
     *                then every held one right behind it. Called after the window has moved, so an
     *                ack sent from inside it already covers them.
     * @return What was done with the frame.
     */
    template <typename F>
    Result OnFrame(SeqNum Seq, const T& Payload, F&& Deliver)
    {
        // Every reliable frame is answered, so a lost ack is repaired by the retransmission
        m_AckPending = true;

        const auto Offset = static_cast<SeqNum>(Seq - m_Expected);
        if (Offset >= 0x8000) { return Result::Duplicate; }
        if (Offset >= m_Window) { return Result::OutOfWindow; }

        if (Offset > 0)
        {
            Slot& S = m_Slots[Offset];
            if (S.Present) { return Result::Duplicate; }

            S.Payload = Payload;
            S.Present = true;
            return Result::Buffered;
        }

        // This one and every frame held right behind it are now in order
        const Slid Passed = Slide(1);
        Deliver(Payload);
        DeliverTaken(Passed, Deliver);
        return Result::Delivered;
    }

    /**
     * @brief The sender has given up on every seq before Oldest (ArqSender::SkipNotice()).
     *
     * Stop waiting for them: what is held before Oldest, and the run held from it on, is delivered
     * in order. Called again for every notice until our ack reaches the sender, so it makes an ack
     * owed too. A notice for a seq already passed is ignored.
     *
     * @param Oldest  The oldest seq the sender still holds.
     * @param Deliver Called as `Deliver(const T&)` for each message that is now in order.
     */
    template <typename F>
    void OnSkip(SeqNum Oldest, F&& Deliver)
    {
        m_AckPending = true;

        const auto Gap = static_cast<SeqNum>(Oldest - m_Expected);
        if (Gap == 0 || Gap >= 0x8000) { return; } // nothing left to skip, or an old notice

        const Slid Passed = Slide(Gap);
        DeliverTaken(Passed, Deliver);
    }

    /** @brief Something was received since the last AckSent(): the peer is owed an ack. */
    bool AckPending() const { return m_AckPending; }
    /** @brief An ack carrying AckNext() and AckSack() has been sent. */
    void AckSent() { m_AckPending = false; }

    /** @brief Cumulative ack: the next seq expected. Every seq before it has been delivered or skipped. */
    SeqNum AckNext() const { return m_Expected; }

    /** @brief Selective ack: bit N set means seq (AckNext() + 1 + N) is held behind a gap. */
    uint16_t AckSack() const
    {
        uint16_t Bits = 0;
        for (size_t i = 1; i < m_Window; ++i)
        {
            if (m_Slots[i].Present) { Bits |= static_cast<uint16_t>(1u << (i - 1)); }
        }
        return Bits;
    }

    /** @brief Forget everything: the peer rebooted and starts numbering from 0 again. */
    void Reset()
    {
        for (size_t i = 0; i < MaxWindow; ++i) { m_Slots[i].Present = false; }
        m_Slots.Reset();
        m_Expected   = 0;
        m_AckPending = false;
    }

private:
    /** @brief One position of the window. */
    struct Slot
    {
        T Payload {};         ///< The held message, valid while Present (and until overwritten, see Slide()).
        bool Present = false; ///< A frame is held here.
    };

    /** @brief What Slide() did. */
    struct Slid
    {
        uint32_t Taken;  ///< Bit i set: position i, counted from the old start, held a frame.
        size_t Advanced; ///< How far the window moved.
    };

    /**
     * @brief Move the window on past Gap seqs that are not held (the one that just arrived, or
     * ones given up on) and every frame held right behind them, taking out what is held on the way.
     *
     * All of it is out of the window before anything is handed over: Deliver may send a frame, and
     * the ack it carries must already cover these, or the sender is left with acked-looking frames
     * it never resends. The payloads stay in the slots until a later OnFrame() stores a new one, so
     * DeliverTaken() can still read them.
     *
     * @return The positions, from the old start, that held a frame, and how far the window moved.
     */
    Slid Slide(size_t Gap)
    {
        uint32_t Taken = 0;
        for (size_t Pos = 0; Pos < Gap && Pos < m_Window; ++Pos)
        {
            if (m_Slots[Pos].Present)
            {
                m_Slots[Pos].Present  = false;
                Taken                |= 1u << Pos;
            }
        }

        size_t Pos = Gap; // seqs given up on beyond the window were never held
        while (Pos < m_Window && m_Slots[Pos].Present)
        {
            m_Slots[Pos].Present  = false;
            Taken                |= 1u << Pos;
            ++Pos;
        }

        m_Slots.Advance(Pos);
        m_Expected = static_cast<SeqNum>(m_Expected + Pos);
        return {.Taken = Taken, .Advanced = Pos};
    }

    /**
     * @brief Hand over, in order, what a Slide() took out.
     *
     * The slide has already moved the start, so what was at position i of the old window is now
     * MaxWindow - Advanced + i places on: a RingWindow keeps what an Advance passed over readable
     * (offsets wrap, so no modulo is needed here).
     */
    template <typename F>
    void DeliverTaken(const Slid& Passed, F& Deliver)
    {
        const size_t Back = Passed.Advanced % MaxWindow;
        for (size_t i = 0; i < m_Window; ++i)
        {
            if ((Passed.Taken & (1u << i)) == 0) { continue; }

            // copy out: Deliver may run long, and the slot is already free
            const T Held = m_Slots[i + MaxWindow - Back].Payload;
            Deliver(Held);
        }
    }

private:
    bricks::RingWindow<Slot, MaxWindow> m_Slots; ///< Offset 0 is m_Expected; only the first m_Window are used.
    uint16_t m_Window;                           ///< ArqReceiverConfig::Window after clamping.
    SeqNum m_Expected = 0;                       ///< The next seq to deliver.
    bool m_AckPending = false;                   ///< See AckPending().
};
} // namespace ra::turtleford
