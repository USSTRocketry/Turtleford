#pragma once

// GoogleMock double for the hardware radio. Every method defaults to a fake (the "delegating
// calls to a fake" pattern), so a test gets the fake's behaviour and can also EXPECT_CALL on the
// same object. An EXPECT_CALL without an action keeps the fake's behaviour; add .WillOnce or
// .WillRepeatedly to override it for those calls.
//
//   MockTelemetryRadio Radio;         delegates to its own FakeTelemetryRadio (Radio.Fake):
//                                     frames sent are kept, frames queued in RxQueue are received
//   MockTelemetryRadio Radio {Sim};   delegates to any other ITelemetryRadio, e.g. a SimRadio
//
// Use NiceMock when the calls are not the point of the test, StrictMock when any call a test did
// not expect is a failure.

#include <cstddef>
#include <cstdint>
#include <span>

#include <gmock/gmock.h>

#include "abstractions/ITelemetryRadio.h"
#include "Sim/FakeTelemetryRadio.h"

namespace mocks
{
class MockTelemetryRadio : public HAL::ITelemetryRadio
{
public:
    MockTelemetryRadio() { DelegateTo(Fake); }
    explicit MockTelemetryRadio(HAL::ITelemetryRadio& Other) { DelegateTo(Other); }

    MOCK_METHOD(bool, begin, (), (override));
    MOCK_METHOD(void, reset, (uint8_t), (override));
    MOCK_METHOD(bool, send, (std::span<const uint8_t>), (override));
    MOCK_METHOD(bool, receive, (std::span<uint8_t>, size_t&), (override));
    MOCK_METHOD(void, setFrequency, (float), (override));
    MOCK_METHOD(void, setTxPower, (uint8_t), (override));
    MOCK_METHOD(void, configureLoRa, (uint8_t, uint16_t, uint8_t), (override));
    MOCK_METHOD(void, setAddress, (const uint8_t), (override));
    MOCK_METHOD(void, setDestinationAddress, (const uint8_t), (override));
    MOCK_METHOD(void, setPromiscuousMode, (bool), (override));
    MOCK_METHOD(void*, native_handle, (), (override));

    // The default behaviour when the constructor was not given another radio
    sim::FakeTelemetryRadio Fake;

private:
    void DelegateTo(HAL::ITelemetryRadio& Radio)
    {
        using testing::_;

        ON_CALL(*this, begin()).WillByDefault([&Radio] { return Radio.begin(); });
        ON_CALL(*this, reset(_)).WillByDefault([&Radio](uint8_t Pin) { Radio.reset(Pin); });
        ON_CALL(*this, send(_)).WillByDefault([&Radio](std::span<const uint8_t> Data) { return Radio.send(Data); });
        ON_CALL(*this, receive(_, _))
            .WillByDefault([&Radio](std::span<uint8_t> Buffer, size_t& Length) { return Radio.receive(Buffer, Length); });
        ON_CALL(*this, setFrequency(_)).WillByDefault([&Radio](float Frequency) { Radio.setFrequency(Frequency); });
        ON_CALL(*this, setTxPower(_)).WillByDefault([&Radio](uint8_t Power) { Radio.setTxPower(Power); });
        ON_CALL(*this, configureLoRa(_, _, _))
            .WillByDefault([&Radio](uint8_t Sf, uint16_t Bw, uint8_t Cr) { Radio.configureLoRa(Sf, Bw, Cr); });
        ON_CALL(*this, setAddress(_)).WillByDefault([&Radio](uint8_t Address) { Radio.setAddress(Address); });
        ON_CALL(*this, setDestinationAddress(_))
            .WillByDefault([&Radio](uint8_t Address) { Radio.setDestinationAddress(Address); });
        ON_CALL(*this, setPromiscuousMode(_)).WillByDefault([&Radio](bool Enable) { Radio.setPromiscuousMode(Enable); });
        ON_CALL(*this, native_handle()).WillByDefault([&Radio] { return Radio.native_handle(); });
    }
};
} // namespace mocks
