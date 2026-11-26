#pragma once

// An in-memory radio link for tests: two SimTransports joined back to back, with
// controllable delay, loss, duplication and channel, and no real time involved.

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "Radio.h"
#include "Transport/Transport.h"

namespace sim
{
using namespace ra::turtleford;

class SimTransport final : public ITransport
{
public:
    /** @brief Return true to lose the message on air. Runs before the random loss. */
    using DropFilter = std::function<bool(std::span<const std::byte>)>;

    explicit SimTransport(TransportKind Kind = TransportKind::Lora, uint32_t Seed = 1) :
        m_Caps {.Kind = Kind, .MaxMessageSize = 512, .RoundTripMs = 100, .ChannelCount = 4}, m_Rng(Seed)
    {
    }

    static void Connect(SimTransport& A, SimTransport& B)
    {
        A.m_Peer = &B;
        B.m_Peer = &A;
    }

    void SetMaxMessageSize(uint16_t Size) { m_Caps.MaxMessageSize = Size; }
    void SetChannelCount(ChannelNumber Count) { m_Caps.ChannelCount = Count; }

    // ---- knobs -------------------------------------------------------------
    uint32_t DelayMs     = 10;   // time on air
    uint32_t LossPercent = 0;    // random loss of what this end sends
    uint32_t DupPercent  = 0;    // random duplication of what this end sends
    bool Ready           = true; // IsReady()
    bool Cut             = false; // nothing this end sends arrives
    size_t FreeSlots     = 64;    // what TxFree() reports
    uint32_t QueueMs     = 0;     // time a message waits in this end's queue before going on air
    DropFilter Drop {};

    // ---- observations ------------------------------------------------------
    uint32_t Sent      = 0; // messages accepted by Send()
    uint32_t Lost      = 0; // of those, how many never arrived
    uint32_t Delivered = 0; // messages handed out by Receive()
    uint32_t BulkSent  = 0; // of the ones accepted by Send(), how many were marked bulk
    bool LastBulk      = false; // the bulk argument of the latest Send()

    // ---- ITransport --------------------------------------------------------
    const TransportCaps& Caps() const override { return m_Caps; }
    bool IsReady() const override { return Ready; }
    bool TxIdle() const override { return m_Leaving.empty(); }
    size_t TxFree() const override { return Ready ? FreeSlots : 0; }
    uint32_t TxCompleted() const override { return m_Completed; }
    void Poll(uint32_t NowMs) override
    {
        m_NowMs = NowMs;
        while (!m_Leaving.empty() && static_cast<int32_t>(NowMs - m_Leaving.front()) >= 0)
        {
            m_Leaving.pop_front();
            ++m_Completed;
        }
    }

    bool Send(std::span<const std::byte> Message, bool Bulk = false) override
    {
        if (!Ready || Message.empty() || Message.size() > m_Caps.MaxMessageSize) { return false; }
        ++Sent;
        BulkSent += Bulk ? 1 : 0; // only recorded: nothing here takes turns
        LastBulk  = Bulk;
        m_Leaving.push_back(m_NowMs + QueueMs);

        const bool Dropped = Cut || m_Peer == nullptr || (Drop && Drop(Message)) || Roll(LossPercent);
        if (Dropped)
        {
            ++Lost;
            return true; // it left the radio, it just never got anywhere
        }

        const int Copies = Roll(DupPercent) ? 2 : 1;
        for (int i = 0; i < Copies; ++i)
        {
            m_Peer->m_Inbox.push_back(
                {.Data = {Message.begin(), Message.end()}, .ArrivesMs = m_NowMs + QueueMs + DelayMs, .Channel = m_Channel});
        }
        return true;
    }

    std::span<const std::byte> Receive(uint32_t NowMs) override
    {
        m_NowMs = NowMs;
        while (!m_Inbox.empty() && static_cast<int32_t>(NowMs - m_Inbox.front().ArrivesMs) >= 0)
        {
            m_Current = std::move(m_Inbox.front());
            m_Inbox.pop_front();

            // only heard if we are listening where it was sent
            if (m_Current.Channel != m_Channel) { continue; }

            ++Delivered;
            return m_Current.Data;
        }
        return {};
    }

    bool SetChannel(ChannelNumber Channel) override
    {
        if (Channel >= m_Caps.ChannelCount) { return false; }
        m_Channel = Channel;
        return true;
    }
    ChannelNumber Channel() const override { return m_Channel; }

private:
    struct InFlight
    {
        std::vector<std::byte> Data;
        uint32_t ArrivesMs = 0;
        ChannelNumber Channel = 0;
    };

    bool Roll(uint32_t Percent) { return Percent > 0 && (m_Rng() % 100) < Percent; }

private:
    TransportCaps m_Caps;
    std::mt19937 m_Rng;
    SimTransport* m_Peer = nullptr;
    std::deque<InFlight> m_Inbox;
    std::deque<uint32_t> m_Leaving; // when each accepted message goes on air
    uint32_t m_Completed = 0;
    InFlight m_Current;
    uint32_t m_NowMs  = 0;
    ChannelNumber m_Channel = 0;
};
} // namespace sim
