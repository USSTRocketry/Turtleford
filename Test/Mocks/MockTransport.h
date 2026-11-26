#pragma once

// GoogleMock double for ITransport. Two ways to build it:
//
//   MockTransport T {Fake};   every call defaults to Fake (a SimTransport, say), so the transport
//                             really carries messages and tests can still EXPECT_CALL on it.
//   MockTransport T {Caps};   a bare stub: ready, accepts nothing useful, never receives. For
//                             tests where only the way a caller treats the transport matters.
//
// An EXPECT_CALL without an action keeps the default behaviour. Use NiceMock for noise,
// StrictMock to prove a transport is left alone.

#include <cstddef>
#include <cstdint>
#include <span>

#include <gmock/gmock.h>

#include "Transport/Transport.h"

namespace mocks
{
class MockTransport : public ra::turtleford::ITransport
{
public:
    explicit MockTransport(ra::turtleford::ITransport& Fake)
    {
        using testing::_;

        ON_CALL(*this, Caps()).WillByDefault([&Fake]() -> const ra::turtleford::TransportCaps& { return Fake.Caps(); });
        ON_CALL(*this, IsReady()).WillByDefault([&Fake] { return Fake.IsReady(); });
        ON_CALL(*this, Send(_)).WillByDefault([&Fake](std::span<const std::byte> Message) { return Fake.Send(Message); });
        ON_CALL(*this, TxFree()).WillByDefault([&Fake] { return Fake.TxFree(); });
        ON_CALL(*this, TxCompleted()).WillByDefault([&Fake] { return Fake.TxCompleted(); });
        ON_CALL(*this, TxIdle()).WillByDefault([&Fake] { return Fake.TxIdle(); });
        ON_CALL(*this, Poll(_)).WillByDefault([&Fake](uint32_t NowMs) { Fake.Poll(NowMs); });
        ON_CALL(*this, Receive(_)).WillByDefault([&Fake](uint32_t NowMs) { return Fake.Receive(NowMs); });
        ON_CALL(*this, SetChannel(_)).WillByDefault([&Fake](ra::turtleford::ChannelNumber Channel) { return Fake.SetChannel(Channel); });
        ON_CALL(*this, Channel()).WillByDefault([&Fake] { return Fake.Channel(); });
    }

    explicit MockTransport(const ra::turtleford::TransportCaps& StubCaps) : m_StubCaps(StubCaps)
    {
        using testing::_;
        using testing::Return;
        using testing::ReturnRef;

        ON_CALL(*this, Caps()).WillByDefault(ReturnRef(m_StubCaps));
        ON_CALL(*this, IsReady()).WillByDefault(Return(true));
        ON_CALL(*this, Send(_)).WillByDefault(Return(true));
        ON_CALL(*this, TxFree()).WillByDefault(Return(1));
        ON_CALL(*this, TxCompleted()).WillByDefault(Return(0));
        ON_CALL(*this, TxIdle()).WillByDefault(Return(true));
        ON_CALL(*this, SetChannel(_)).WillByDefault(Return(false));
        ON_CALL(*this, Channel()).WillByDefault(Return(0));
    }

    MOCK_METHOD(const ra::turtleford::TransportCaps&, Caps, (), (const, override));
    MOCK_METHOD(bool, IsReady, (), (const, override));
    // ITransport::Send takes the bulk flag too. It is recorded here, and the mocked call is the one
    // with just the message, so tests that only care about messages say Send(_).
    bool Send(std::span<const std::byte> Message, bool Bulk) override
    {
        LastBulk = Bulk;
        ++(Bulk ? BulkSends : PlainSends);
        return Send(Message);
    }
    MOCK_METHOD(bool, Send, (std::span<const std::byte>));
    MOCK_METHOD(size_t, TxFree, (), (const, override));
    MOCK_METHOD(uint32_t, TxCompleted, (), (const, override));
    MOCK_METHOD(bool, TxIdle, (), (const, override));
    MOCK_METHOD(void, Poll, (uint32_t), (override));
    MOCK_METHOD(std::span<const std::byte>, Receive, (uint32_t), (override));
    MOCK_METHOD(bool, SetChannel, (ra::turtleford::ChannelNumber), (override));
    MOCK_METHOD(ra::turtleford::ChannelNumber, Channel, (), (const, override));

    bool LastBulk       = false; // the bulk argument of the latest Send()
    uint32_t BulkSends  = 0;     // Send() calls with the bulk flag, and without it
    uint32_t PlainSends = 0;

private:
    ra::turtleford::TransportCaps m_StubCaps {};
};
} // namespace mocks
