/**
 * @file LoraTransport.cpp
 * @brief LoraTransport: queueing, fragmenting and turn taking around the radio. See Transport/LoraTransport.h.
 */

#include "Transport/LoraTransport.h"

#include <algorithm>

namespace ra::turtleford
{
LoraTransport::LoraTransport(HAL::ITelemetryRadio& Radio, const LoraTransportConfig& Config) :
    m_Radio(Radio),
    m_ChannelsMHz(Config.ChannelsMHz),
    m_Fragmenter(Config.Address, std::min(Config.FrameSize, MaxFrameSize)),
    m_TxQueue(std::min<size_t>(Config.MaxMessageSize, m_Fragmenter.MaxMessageSize())),
    m_Reassembler(Config.PeerAddress, Config.MaxMessageSize, Config.ReassemblyTimeoutMs),
    m_Peer(Config.PeerAddress),
    m_Turns(TurnTakingConfig {
        .ListenEveryMs = Config.ListenEveryMs,
        .ListenMs      = Config.ListenMs,
        .GuardMs       = Config.ListenGuardMs,
        // It can be silent for a whole window and then a message while it is still in the middle of a stream
        .QuietMs = Config.ListenMs + Config.ListenMs / config::lora::ListenJitterDivisor + Config.RoundTripMs / 2,
        // The two ends have different addresses, so they draw different window lengths
        .Seed = Config.Address,
    })
{
    // The radio itself drops frames addressed to somebody else; frames from somebody else are
    // dropped by the reassembler, which checks the sender address in our own header.
    m_Radio.setAddress(Config.Address);
    m_Radio.setDestinationAddress(Config.PeerAddress);
    m_Radio.setPromiscuousMode(false);

    m_Caps = TransportCaps {
        .Kind           = TransportKind::Lora,
        .MaxMessageSize = static_cast<uint16_t>(m_TxQueue.MaxPacketSize()),
        .RoundTripMs    = Config.RoundTripMs,
        .ChannelCount   = static_cast<ChannelNumber>(m_ChannelsMHz.size()),
    };

    // make sure the radio and the table agree from the start
    m_Channel = Config.InitialChannel < m_Caps.ChannelCount ? Config.InitialChannel : 0;
    if (m_Caps.ChannelCount > 0) { m_Radio.setFrequency(m_ChannelsMHz[m_Channel]); }
}

bool LoraTransport::Send(std::span<const std::byte> Bytes, bool Bulk)
{
    // rejects an empty or oversized message, and one there is no room for
    return m_TxQueue.Push(Bytes, Bulk);
}

void LoraTransport::Poll(uint32_t NowMs)
{
    m_Turns.Advance(NowMs);

    if (m_Fragmenter.Done())
    {
        if (m_TxQueue.Empty() || !m_Turns.MayTransmit()) { return; }

        // The marks go on the last fragment; none at all while turn taking is off
        uint8_t Flags = 0;
        if (m_Turns.Enabled())
        {
            const bool Bulk = m_TxQueue.FrontBulk();
            const bool Over = m_Turns.MarkOver(NowMs, Bulk, m_TxQueue.Size() == 1);
            Flags = static_cast<uint8_t>((Bulk ? FragmentHeader::FlagBulk : 0) | (Over ? FragmentHeader::FlagOver : 0));
        }

        // The message stays in its queue slot until Dequeue, so the fragmenter reads it in place
        if (!m_Fragmenter.Begin(m_TxQueue.Front(), Flags))
        {
            m_TxQueue.Pop();
            ++m_TxCompleted;
            return;
        }
    }

    // One frame per poll: the HAL blocks for the whole time on air
    const auto Frame = m_Fragmenter.Current();
    if (!m_Radio.send({reinterpret_cast<const uint8_t*>(Frame.data()), Frame.size()}))
    {
        // radio busy, same frame again next time
        return;
    }

    m_Fragmenter.Advance();
    if (m_Fragmenter.Done())
    {
        m_TxQueue.Pop();
        ++m_TxCompleted;
        // The HAL blocked until the frame was off the air; the window starts at the next call
        m_Turns.Sent((m_Fragmenter.Flags() & FragmentHeader::FlagOver) != 0);
    }
}

std::span<const std::byte> LoraTransport::Receive(uint32_t NowMs)
{
    m_Turns.Advance(NowMs);

    for (int i = 0; i < config::lora::FramesPerReceive; ++i)
    {
        size_t Size = 0;
        if (!m_Radio.receive(std::span {m_RxFrame}, Size) || Size == 0) { return {}; }

        Size = std::min(Size, m_RxFrame.size());

        const std::span<const std::byte> Frame {reinterpret_cast<const std::byte*>(m_RxFrame.data()), Size};

        const auto Header = FragmentHeader::Parse(Frame);
        if (Header && Header->Source == m_Peer)
        {
            m_Turns.Heard(NowMs);
            // Counted even if the message turns out to be broken: the peer has stopped talking all the same
            if (Header->IsLast()) { m_Turns.PeerFinished(NowMs, Header->IsOver(), Header->IsBulk()); }
        }

        const auto Message = m_Reassembler.Push(Frame, NowMs);
        if (!Message.empty()) { return Message; }
    }
    return {};
}

bool LoraTransport::SetChannel(ChannelNumber Channel)
{
    if (Channel >= m_Caps.ChannelCount) { return false; }

    m_Channel = Channel;
    m_Radio.setFrequency(m_ChannelsMHz[Channel]);
    return true;
}
} // namespace ra::turtleford
