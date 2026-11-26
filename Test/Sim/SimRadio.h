#pragma once

// A simulated LoRa radio (HAL::ITelemetryRadio) and the air between two or more of them.
//
// What it models, because the stack has to live with it on the real RFM95 + RadioHead driver:
//   - airtime: a frame occupies the air for OverheadMs + bytes * PerByteMs
//   - send() blocks: it moves the air's clock forward by the airtime before it returns
//   - half duplex: a radio hears nothing that is on air while it transmits itself
//   - collisions: two frames overlapping in time on one frequency are both lost
//   - one receive buffer: a frame finishing while the buffer is still full is lost (overrun)
//   - deaf after transmit: after send() the radio does not listen until receive() is called
//     (DeafAfterTransmit, on by default; unverified against the real driver)
//   - frequency: a frame is only heard by a radio tuned to the frequency it was sent on
//   - addresses: a frame is only kept by the radio it is addressed to, unless that radio is
//     promiscuous (255 is broadcast, and what a radio that was never programmed uses)
//
// There is no real time and no thread. The clock is RadioAir::Now(); whoever drives the radios
// sets it to the time of the caller before each call (see LoraRig.h) and the radios read it.
// Frames are judged lazily, when the receiving radio is next called, so the callers must run in
// time order: a frame has to be sent before any call at a later time is made on the receiver.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include "abstractions/ITelemetryRadio.h"

namespace sim
{
class SimRadio;

struct AirConfig
{
    uint32_t OverheadMs = 10;  // preamble and header, whatever the frame size
    double PerByteMs    = 0.5; // payload cost, rounded up to whole milliseconds per frame
    uint32_t Seed       = 1;
};

struct AirFrame
{
    std::vector<uint8_t> Data;
    uint32_t StartMs = 0;
    uint32_t EndMs   = 0;
    float Frequency  = 0.0f;
    size_t Sender    = 0;
    uint8_t To       = 255; // destination address, 255 is broadcast
    bool Collided    = false;
};

class RadioAir
{
public:
    /** @brief Return true to lose the frame. Runs before the random loss. */
    using DropFilter = std::function<bool(std::span<const uint8_t> Frame, size_t Sender)>;

    struct Counters
    {
        uint32_t FramesSent         = 0;
        uint32_t Delivered          = 0; // taken out of a radio by receive()
        uint32_t LostCollision      = 0; // frames that overlapped another; counted once per frame
        uint32_t LostOverrun        = 0; // finished while the receive buffer was still full
        uint32_t LostDeaf           = 0; // finished while the radio was not listening after a send
        uint32_t LostTransmitting   = 0; // on air while the radio was transmitting itself
        uint32_t LostWrongFrequency = 0; // nobody tuned to it
        uint32_t LostWrongAddress   = 0; // heard by a radio it was not addressed to
        uint32_t LostRandom         = 0;
        uint32_t LostFiltered       = 0;
        uint32_t LostCut            = 0;
    };

    explicit RadioAir(const AirConfig& Config = {}) : m_Config(Config), m_Rng(Config.Seed) {}

    RadioAir(const RadioAir&)            = delete;
    RadioAir& operator=(const RadioAir&) = delete;

    // ---- clock -------------------------------------------------------------
    uint32_t Now() const { return m_NowMs; }
    void SetNow(uint32_t NowMs) { m_NowMs = NowMs; }

    uint32_t AirtimeMs(size_t Bytes) const
    {
        return m_Config.OverheadMs + static_cast<uint32_t>(std::ceil(static_cast<double>(Bytes) * m_Config.PerByteMs));
    }

    // ---- knobs -------------------------------------------------------------
    uint32_t LossPercent = 0;     // random loss of any frame
    bool Cut             = false; // nothing sent is heard (it still occupies the air)
    DropFilter Drop {};

    Counters Stats {};

    // ---- used by SimRadio --------------------------------------------------
    size_t Attach(SimRadio& Radio)
    {
        m_Radios.push_back(&Radio);
        return m_Radios.size() - 1;
    }

    /** @brief Put a frame on air starting now; returns the time it ends. */
    inline uint32_t Transmit(size_t Sender, std::span<const uint8_t> Data, float Frequency, uint8_t To);

private:
    bool Roll(uint32_t Percent) { return Percent > 0 && (m_Rng() % 100) < Percent; }

private:
    AirConfig m_Config;
    std::mt19937 m_Rng;
    uint32_t m_NowMs = 0;
    std::vector<SimRadio*> m_Radios;
    std::vector<std::shared_ptr<AirFrame>> m_History; // recent frames, for collision checks
};

class SimRadio final : public HAL::ITelemetryRadio
{
public:
    /** @brief Largest packet the RadioHead RF95 driver carries. */
    static constexpr size_t MaxPayload = 251;

    struct Counters
    {
        uint32_t Sent             = 0;
        uint32_t Received         = 0; // taken by receive()
        uint32_t Overruns         = 0;
        uint32_t LostDeaf         = 0;
        uint32_t LostTransmitting = 0;
        uint32_t LostWrongAddress = 0;
        uint32_t FrequencyChanges = 0;
    };

    explicit SimRadio(RadioAir& Air) : m_Air(Air), m_Index(Air.Attach(*this)) {}

    SimRadio(const SimRadio&)            = delete;
    SimRadio& operator=(const SimRadio&) = delete;

    // ---- knobs and observations -------------------------------------------
    bool DeafAfterTransmit = true; // after send() hear nothing until receive() is called
    Counters Stats {};

    size_t Index() const { return m_Index; }
    float Frequency() const { return m_Frequency; }

    // ---- HAL::ITelemetryRadio ---------------------------------------------
    bool begin() override { return true; }
    void reset(uint8_t) override {}

    bool send(std::span<const uint8_t> Data) override
    {
        if (Data.empty() || Data.size() > MaxPayload) { return false; }

        const uint32_t Start = m_Air.Now();
        Settle(Start); // what ended before we began is judged while we were still listening

        // An open deaf spell ends here: the transmission itself covers the time from now on
        if (!m_Gaps.empty() && m_Gaps.back().EndMs == Forever) { m_Gaps.back().EndMs = Start; }

        const uint32_t End = m_Air.Transmit(m_Index, Data, m_Frequency, m_Destination);
        m_Gaps.push_back({.StartMs = Start, .EndMs = End, .Transmitting = true});
        if (DeafAfterTransmit) { m_Gaps.push_back({.StartMs = End, .EndMs = Forever, .Transmitting = false}); }

        m_Air.SetNow(End); // blocking: back only once it is off the air
        ++Stats.Sent;
        return true;
    }

    bool receive(std::span<uint8_t> Buffer, size_t& ReceivedLength) override
    {
        const uint32_t Now = m_Air.Now();

        // listening again from now; a frame that began earlier was only half heard
        if (!m_Gaps.empty() && m_Gaps.back().EndMs == Forever) { m_Gaps.back().EndMs = Now; }
        Settle(Now);

        ReceivedLength = 0;
        if (m_Held.empty()) { return false; }

        ReceivedLength = std::min(m_Held.size(), Buffer.size());
        std::copy_n(m_Held.begin(), ReceivedLength, Buffer.begin());
        m_Held.clear();

        ++Stats.Received;
        ++m_Air.Stats.Delivered;
        return true;
    }

    void setFrequency(float Frequency) override
    {
        Settle(m_Air.Now()); // frames that finished earlier were heard on the old frequency
        m_Frequency = Frequency;
        ++Stats.FrequencyChanges;
    }

    void setTxPower(uint8_t) override {}
    void configureLoRa(uint8_t, uint16_t, uint8_t) override {}
    void setAddress(const uint8_t Address) override { m_Address = Address; }
    void setDestinationAddress(const uint8_t Address) override { m_Destination = Address; }
    void setPromiscuousMode(bool Enable) override { m_Promiscuous = Enable; }
    void* native_handle() override { return nullptr; }

    // ---- used by RadioAir -------------------------------------------------
    void OnAir(std::shared_ptr<AirFrame> Frame) { m_Pending.push_back(std::move(Frame)); }

private:
    static constexpr uint32_t Forever  = std::numeric_limits<uint32_t>::max();
    static constexpr uint8_t Broadcast = 255;

    // A stretch of time in which the radio could not hear
    struct Gap
    {
        uint32_t StartMs;
        uint32_t EndMs;
        bool Transmitting; // otherwise deaf after a transmit
    };

    // Decide the fate of every frame that has finished by Now
    void Settle(uint32_t Now)
    {
        std::stable_sort(m_Pending.begin(),
                         m_Pending.end(),
                         [](const auto& A, const auto& B) { return A->EndMs < B->EndMs; });

        size_t Done = 0;
        for (; Done < m_Pending.size() && m_Pending[Done]->EndMs <= Now; ++Done) { Judge(*m_Pending[Done]); }
        m_Pending.erase(m_Pending.begin(), m_Pending.begin() + static_cast<std::ptrdiff_t>(Done));

        // forget old gaps, no frame lasts this long
        std::erase_if(m_Gaps, [Now](const Gap& G) { return G.EndMs != Forever && G.EndMs + 10000 < Now; });
    }

    void Judge(const AirFrame& Frame)
    {
        if (Frame.Frequency != m_Frequency)
        {
            ++m_Air.Stats.LostWrongFrequency;
            return;
        }
        if (Frame.Collided) { return; } // counted when it happened

        // the driver throws away what is addressed to somebody else before it reaches the buffer
        if (!m_Promiscuous && Frame.To != m_Address && Frame.To != Broadcast)
        {
            ++Stats.LostWrongAddress;
            ++m_Air.Stats.LostWrongAddress;
            return;
        }

        for (const Gap& G : m_Gaps)
        {
            if (G.StartMs >= Frame.EndMs || Frame.StartMs >= G.EndMs) { continue; }

            if (G.Transmitting)
            {
                ++Stats.LostTransmitting;
                ++m_Air.Stats.LostTransmitting;
            }
            else
            {
                ++Stats.LostDeaf;
                ++m_Air.Stats.LostDeaf;
            }
            return;
        }

        if (!m_Held.empty())
        {
            ++Stats.Overruns;
            ++m_Air.Stats.LostOverrun;
            return;
        }
        m_Held = Frame.Data;
    }

private:
    RadioAir& m_Air;
    size_t m_Index;
    float m_Frequency = 0.0f;
    uint8_t m_Address     = Broadcast;
    uint8_t m_Destination = Broadcast;
    bool m_Promiscuous    = false;

    std::vector<std::shared_ptr<AirFrame>> m_Pending; // on air or just finished, not yet judged
    std::vector<Gap> m_Gaps;
    std::vector<uint8_t> m_Held; // the single receive buffer; empty when free
};

inline uint32_t RadioAir::Transmit(size_t Sender, std::span<const uint8_t> Data, float Frequency, uint8_t To)
{
    auto Frame       = std::make_shared<AirFrame>();
    Frame->Data      = {Data.begin(), Data.end()};
    Frame->StartMs   = m_NowMs;
    Frame->EndMs     = m_NowMs + AirtimeMs(Data.size());
    Frame->Frequency = Frequency;
    Frame->Sender    = Sender;
    Frame->To        = To;
    ++Stats.FramesSent;

    // Frames on the same frequency that share any time on air destroy each other
    for (const auto& Other : m_History)
    {
        if (Other->Frequency != Frame->Frequency) { continue; }
        if (Other->StartMs >= Frame->EndMs || Frame->StartMs >= Other->EndMs) { continue; }

        if (!Other->Collided) { ++Stats.LostCollision; }
        if (!Frame->Collided) { ++Stats.LostCollision; }
        Other->Collided = true;
        Frame->Collided = true;
    }

    std::erase_if(m_History, [&](const auto& F) { return F->EndMs + 1000 < Frame->StartMs; });
    m_History.push_back(Frame);

    const bool Filtered = Drop && Drop(Data, Sender);
    const bool Random   = Roll(LossPercent);
    if (Cut) { ++Stats.LostCut; }
    else if (Filtered) { ++Stats.LostFiltered; }
    else if (Random) { ++Stats.LostRandom; }
    else
    {
        for (size_t i = 0; i < m_Radios.size(); ++i)
        {
            if (i != Sender) { m_Radios[i]->OnAir(Frame); }
        }
    }
    return Frame->EndMs;
}
} // namespace sim
