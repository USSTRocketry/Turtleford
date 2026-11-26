#pragma once

// A HAL radio with a plain in-memory behaviour: it keeps the frames that were sent and hands out
// the frames a test queued for it. What the radio was told to do (frequency, addresses, refusing
// a send) is the business of the mock around it. No airtime, no collisions (see SimRadio.h for those).
//
// Tests normally reach it through Mocks/MockTelemetryRadio.h, which delegates every call here
// and so adds call expectations on top of this behaviour.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "abstractions/ITelemetryRadio.h"

namespace sim
{
class FakeTelemetryRadio final : public HAL::ITelemetryRadio
{
public:
    std::vector<std::vector<uint8_t>> Sent;   // every frame that went on air, oldest first
    std::deque<std::vector<uint8_t>> RxQueue; // frames receive() will hand out, oldest first

    bool begin() override { return true; }
    void reset(uint8_t) override {}

    bool send(std::span<const uint8_t> Data) override
    {
        Sent.emplace_back(Data.begin(), Data.end());
        return true;
    }

    bool receive(std::span<uint8_t> Buffer, size_t& ReceivedLength) override
    {
        ReceivedLength = 0;
        if (RxQueue.empty()) { return false; }

        const auto& Frame = RxQueue.front();
        ReceivedLength    = std::min(Frame.size(), Buffer.size());
        std::copy_n(Frame.begin(), ReceivedLength, Buffer.begin());
        RxQueue.pop_front();
        return true;
    }

    void setFrequency(float) override {}
    void setTxPower(uint8_t) override {}
    void configureLoRa(uint8_t, uint16_t, uint8_t) override {}
    void setAddress(const uint8_t) override {}
    void setDestinationAddress(const uint8_t) override {}
    void setPromiscuousMode(bool) override {}
    void* native_handle() override { return nullptr; }
};
} // namespace sim
