/**
 * @file Fragment.cpp
 * @brief FragmentHeader, Fragmenter and Reassembler. See Transport/Fragment.h.
 */

#include "Transport/Fragment.h"

#include <algorithm>
#include <cstring>

#include "Clock.h"

namespace ra::turtleford
{
namespace
{
/** @brief Smallest frame size a Fragmenter accepts: the header and one payload byte. */
constexpr uint16_t MinFrameSize = FragmentHeader::Size + 1;
} // namespace

std::optional<FragmentHeader> FragmentHeader::Parse(std::span<const std::byte> Frame)
{
    if (Frame.size() < Size) { return std::nullopt; }

    return FragmentHeader {
        .Source    = std::to_integer<uint8_t>(Frame[0]),
        .MessageId = std::to_integer<uint8_t>(Frame[1]),
        .Index     = std::to_integer<uint8_t>(Frame[2]),
        .Count     = std::to_integer<uint8_t>(Frame[3]),
        .Flags     = std::to_integer<uint8_t>(Frame[4]),
    };
}

Fragmenter::Fragmenter(uint8_t Source, uint16_t FrameSize) :
    m_FrameSize(std::max(FrameSize, MinFrameSize)),
    m_Frame(std::make_unique<std::byte[]>(m_FrameSize)),
    m_Source(Source)
{
}

bool Fragmenter::Begin(std::span<const std::byte> Bytes, uint8_t Flags)
{
    if (Bytes.empty() || Bytes.size() > MaxMessageSize()) { return false; }

    m_Message = Bytes;
    m_Count   = static_cast<uint8_t>((Bytes.size() + PayloadPerFrame() - 1) / PayloadPerFrame());
    m_Index   = 0;
    m_Flags   = Flags;
    ++m_MessageId;

    return true;
}

std::span<const std::byte> Fragmenter::Current()
{
    if (Done()) { return {}; }

    const size_t Offset = m_Index * PayloadPerFrame();
    const size_t Length = std::min(PayloadPerFrame(), m_Message.size() - Offset);

    m_Frame[0] = std::byte {m_Source};
    m_Frame[1] = std::byte {m_MessageId};
    m_Frame[2] = std::byte {m_Index};
    m_Frame[3] = std::byte {m_Count};
    // the marks go on the last fragment only: that is where the sender stops talking
    m_Frame[4] = std::byte {static_cast<uint8_t>(m_Index + 1 == m_Count ? m_Flags : 0)};
    std::memcpy(m_Frame.get() + FragmentHeader::Size, m_Message.data() + Offset, Length);

    return {m_Frame.get(), FragmentHeader::Size + Length};
}

void Fragmenter::Advance()
{
    if (!Done()) { ++m_Index; }
    if (Done()) { m_Message = {}; }
}

Reassembler::Reassembler(uint8_t Peer, uint16_t MaxMessageSize, uint32_t TimeoutMs) :
    m_Capacity(MaxMessageSize),
    m_Buffer(std::make_unique<std::byte[]>(m_Capacity)),
    m_TimeoutMs(TimeoutMs),
    m_Peer(Peer)
{
}

std::span<const std::byte> Reassembler::Drop()
{
    ++m_Dropped;
    return {};
}

std::span<const std::byte> Reassembler::Push(std::span<const std::byte> Frame, uint32_t NowMs)
{
    const auto Parsed = FragmentHeader::Parse(Frame);
    if (!Parsed || Frame.size() <= FragmentHeader::Size) { return Drop(); }

    const FragmentHeader& Header = *Parsed;
    const auto Payload           = Frame.subspan(FragmentHeader::Size);

    if (Header.Source != m_Peer || Header.Count == 0 || Header.Index >= Header.Count)
    {
        return Drop();
    }

    // a half built message whose next fragment never came
    if (m_Active && Elapsed(NowMs, m_LastFrameMs) > m_TimeoutMs) { m_Active = false; }

    if (Header.Index == 0)
    {
        // first fragment always starts over, whatever was in progress is lost
        m_Active    = true;
        m_MessageId = Header.MessageId;
        m_Count     = Header.Count;
        m_NextIndex = 0;
        m_Size      = 0;
    }
    else if (!m_Active || Header.MessageId != m_MessageId || Header.Count != m_Count)
    {
        // middle of a message we never saw the start of
        m_Active = false;
        return Drop();
    }
    else if (Header.Index < m_NextIndex)
    {
        // repeat of a fragment we already have
        return {};
    }
    else if (Header.Index != m_NextIndex)
    {
        // a fragment went missing, the message cannot be completed
        m_Active = false;
        return Drop();
    }

    if (m_Size + Payload.size() > m_Capacity)
    {
        m_Active = false;
        return Drop();
    }

    std::memcpy(m_Buffer.get() + m_Size, Payload.data(), Payload.size());
    m_Size        += Payload.size();
    m_LastFrameMs  = NowMs;
    ++m_NextIndex;

    if (m_NextIndex < m_Count) { return {}; }

    m_Active = false;
    return {m_Buffer.get(), m_Size};
}
} // namespace ra::turtleford
