/**
 * @file Fragment.h
 * @brief Chunking and reassembly shared by transports whose frames are smaller than a message.
 *
 * Every frame on air is: `[Source][MessageId][Index][Count][Flags][payload...]`
 *
 * There is no retransmission at this level. A message that loses any fragment is dropped
 * whole; reliable messages are then resent by Radio.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace ra::turtleford
{
/** @brief The five bytes at the start of every frame, in the order of the members. */
struct FragmentHeader
{
    /** @brief Bytes the header takes at the start of a frame. */
    static constexpr size_t Size      = 5;
    /** @brief Most fragments one message can be cut into (Count is one byte). */
    static constexpr size_t MaxFrames = 255;

    uint8_t Source;    ///< Address of the sender; frames from anyone but the peer are rejected.
    uint8_t MessageId; ///< Same for every fragment of one message.
    uint8_t Index;     ///< 0 .. Count-1.
    uint8_t Count;     ///< Number of fragments in the message.
    uint8_t Flags;     ///< FlagOver and FlagBulk, only meaningful on the last fragment.

    /** @brief The sender has said what it had to say and listens after this message: answer now. */
    static constexpr uint8_t FlagOver = 0x01;
    /** @brief One of a continuous stream, with more right behind it and no answer expected. */
    static constexpr uint8_t FlagBulk = 0x02;

    /**
     * @brief The header at the start of a frame, or nothing if the frame is too short to hold one.
     * @param Frame A whole received frame. Only the header is looked at; its values are not checked.
     */
    static std::optional<FragmentHeader> Parse(std::span<const std::byte> Frame);

    /** @brief The final fragment of its message: the sender has finished talking for now. */
    bool IsLast() const { return Count != 0 && Index == Count - 1; }

    /** @brief The last fragment of a message after which the sender listens. */
    bool IsOver() const { return IsLast() && (Flags & FlagOver) != 0; }

    /** @brief The last fragment of a message that belongs to a stream. */
    bool IsBulk() const { return IsLast() && (Flags & FlagBulk) != 0; }
};

/**
 * @brief Cuts one message at a time into frames.
 *
 * Begin() takes a message, then Current() and Advance() walk through its frames until Done().
 */
class Fragmenter
{
public:
    /**
     * @param Source    Our own address, stamped on every frame.
     * @param FrameSize Largest frame the radio carries, header included. Raised to
     *                  FragmentHeader::Size + 1 if smaller, so a frame holds at least one payload byte.
     */
    Fragmenter(uint8_t Source, uint16_t FrameSize);

    /** @brief Largest message in bytes it can cut up: payload per frame times FragmentHeader::MaxFrames. */
    size_t MaxMessageSize() const { return PayloadPerFrame() * FragmentHeader::MaxFrames; }

    /**
     * @brief Start on a new message. Bytes must stay alive until Done(). Drops any message in progress.
     * @param Bytes The message. Not copied: each frame is cut from it as Current() is called.
     * @param Flags FragmentHeader::FlagOver and FlagBulk, put on the last fragment of the message.
     * @return False if the message is empty or too large.
     */
    bool Begin(std::span<const std::byte> Bytes, uint8_t Flags = 0);

    /** @brief True when there is no frame left to send. */
    bool Done() const { return m_Index >= m_Count; }

    /** @brief The flags given to the last Begin(). Still valid once Done(). */
    uint8_t Flags() const { return m_Flags; }

    /** @brief The frame to send now. Valid until Advance() or Begin(). */
    std::span<const std::byte> Current();

    /** @brief Call once Current() has been sent. */
    void Advance();

private:
    /** @brief Message bytes that fit in one frame after the header. */
    size_t PayloadPerFrame() const { return m_FrameSize - FragmentHeader::Size; }

private:
    size_t m_FrameSize;                      ///< Largest frame, header included.
    std::unique_ptr<std::byte[]> m_Frame;    ///< What Current() builds and returns: m_FrameSize bytes, allocated once.
    std::span<const std::byte> m_Message {}; ///< The message being cut up; empty when none.
    uint8_t m_Source;                        ///< Our address, put in every header.
    uint8_t m_MessageId = 0;                 ///< Id of the current message: counts up by one per Begin(), wrapping.
    uint8_t m_Index     = 0;                 ///< Index of the frame Current() builds next.
    uint8_t m_Count     = 0;                 ///< Frames in the current message.
    uint8_t m_Flags     = 0;                 ///< As given to Begin(). Only the last frame carries them.
};

/**
 * @brief Puts frames from Fragmenter back together.
 *
 * Holds one message at a time. A first fragment (Index 0) always starts a new message. A message
 * that loses a fragment, or whose next fragment is late, is dropped whole.
 */
class Reassembler
{
public:
    /**
     * @param Peer           The only sender address frames are accepted from.
     * @param MaxMessageSize Largest message in bytes it will put together; a bigger one is dropped.
     * @param TimeoutMs      A message in progress is abandoned if its next frame arrives more than this many
     *                       ms after the previous one.
     */
    Reassembler(uint8_t Peer, uint16_t MaxMessageSize, uint32_t TimeoutMs);

    /**
     * @brief Feed one received frame.
     * @param Frame A whole received frame, header included.
     * @param NowMs Current time.
     * @return The whole message once its last fragment arrives, otherwise an empty span.
     *         Valid until the next Push().
     */
    std::span<const std::byte> Push(std::span<const std::byte> Frame, uint32_t NowMs);

    /** @brief Frames thrown away: not from the peer, malformed, or part of an incomplete message. */
    uint32_t Dropped() const { return m_Dropped; }

private:
    /** @brief Count a frame as thrown away; nothing to hand back. @return An empty span. */
    std::span<const std::byte> Drop();

    size_t m_Capacity;                     ///< Largest message in bytes.
    std::unique_ptr<std::byte[]> m_Buffer; ///< The message so far: m_Capacity bytes, allocated once.
    size_t m_Size = 0;                     ///< Bytes of the message in m_Buffer.
    uint32_t m_TimeoutMs;                  ///< Longest gap between two frames of one message.
    uint32_t m_LastFrameMs = 0;            ///< When the last frame of the message in progress arrived.
    uint32_t m_Dropped     = 0;            ///< Frames thrown away so far.
    uint8_t m_Peer;                        ///< The only accepted sender address.
    uint8_t m_MessageId = 0;               ///< Id of the message in progress.
    uint8_t m_NextIndex = 0;               ///< Index of the fragment expected next.
    uint8_t m_Count     = 0;               ///< Fragments in the message in progress.
    bool m_Active       = false;           ///< A message is in progress.
};
} // namespace ra::turtleford
