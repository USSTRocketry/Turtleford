#pragma once

// GoogleMock double for the application's IRadioListener. Every callback defaults to a
// sim::Recorder, which copies what it is shown (message data is only valid during the call), so
// tests can wait on and inspect Rec while also putting EXPECT_CALLs on the callbacks.
//
// OnDelivered, OnFailed, OnLinkChanged and OnChannelChanged are mocked too, which is why
// NiceMock is the usual wrapper: most tests do not care about every callback.

#include <gmock/gmock.h>

#include "Radio.h"
#include "Sim/Recorder.h"

namespace mocks
{
class MockRadioListener : public ra::turtleford::IRadioListener
{
public:
    MockRadioListener()
    {
        using testing::_;

        ON_CALL(*this, OnMessage(_, _))
            .WillByDefault([this](const ra::turtleford::Message& Msg, const ra::turtleford::RxInfo& Info)
                           { Rec.OnMessage(Msg, Info); });
        ON_CALL(*this, OnDelivered(_)).WillByDefault([this](ra::turtleford::Ticket Id) { Rec.OnDelivered(Id); });
        ON_CALL(*this, OnFailed(_)).WillByDefault([this](ra::turtleford::Ticket Id) { Rec.OnFailed(Id); });
        ON_CALL(*this, OnLinkChanged(_, _))
            .WillByDefault([this](ra::turtleford::TransportKind Kind, bool Up) { Rec.OnLinkChanged(Kind, Up); });
        ON_CALL(*this, OnChannelChanged(_, _))
            .WillByDefault([this](ra::turtleford::TransportKind Kind, ra::turtleford::ChannelNumber Channel)
                           { Rec.OnChannelChanged(Kind, Channel); });
        ON_CALL(*this, OnPeerRestarted(_))
            .WillByDefault([this](ra::turtleford::TransportKind Kind) { Rec.OnPeerRestarted(Kind); });
    }

    MOCK_METHOD(void, OnMessage, (const ra::turtleford::Message&, const ra::turtleford::RxInfo&), (override));
    MOCK_METHOD(void, OnDelivered, (ra::turtleford::Ticket), (override));
    MOCK_METHOD(void, OnFailed, (ra::turtleford::Ticket), (override));
    MOCK_METHOD(void, OnLinkChanged, (ra::turtleford::TransportKind, bool), (override));
    MOCK_METHOD(void, OnChannelChanged, (ra::turtleford::TransportKind, ra::turtleford::ChannelNumber), (override));
    MOCK_METHOD(void, OnPeerRestarted, (ra::turtleford::TransportKind), (override));

    // What the callbacks have reported so far
    sim::Recorder Rec;
};
} // namespace mocks
