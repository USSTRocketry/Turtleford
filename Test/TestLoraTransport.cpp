#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "Transport/LoraTransport.h"
#include "abstractions/ITelemetryRadio.h"
#include <memory>
#include <vector>

using namespace ra::turtleford;
using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

class MockRadio : public ITelemetryRadio
{
public:
    MOCK_METHOD(bool, begin, (), (override));
    MOCK_METHOD(void, reset, (uint8_t), (override));
    MOCK_METHOD(bool, send, (const uint8_t*, size_t), (override));
    MOCK_METHOD(bool, receive, (uint8_t*, size_t, size_t&), (override));
    MOCK_METHOD(void, setFrequency, (float), (override));
    MOCK_METHOD(void, setTxPower, (uint8_t), (override));
    MOCK_METHOD(void, configureLoRa, (uint8_t, uint16_t, uint8_t), (override));
    MOCK_METHOD(void, setAddress, (const uint8_t), (override));
    MOCK_METHOD(void, setDestinationAddress, (const uint8_t), (override));
    MOCK_METHOD(void, setPromiscuousMode, (bool), (override));
    MOCK_METHOD(void*, native_handle, (), (override));
};

TEST(LoraTransportTest, CreateSessionAndLifecycle)
{
    auto Radio   = std::make_unique<NiceMock<MockRadio>>();
    // Keep a raw pointer if we need to set expectations before move
    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Config  = std::make_unique<LoraTransferConfig>(10, nullptr);
    auto Session = Manager->CreateSession(std::move(Config));

    ASSERT_NE(Session, nullptr);
    EXPECT_TRUE(Session->IsOpen());
    EXPECT_EQ(Session->Transport(), TransportType::Lora);

    Manager->Close(*Session);
    EXPECT_FALSE(Session->IsOpen()); // Closed by manager
}

TEST(LoraTransportTest, SendData)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();

    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));

    std::vector<std::byte> Data = {std::byte {0x01}, std::byte {0x02}, std::byte {0x03}};

    // Expect radio send to be called when Process is run
    EXPECT_CALL(*RadioPtr, send(_, _)).Times(1).WillOnce(Return(true));

    EXPECT_TRUE(Session->Send(Data));

    Manager->Process();
}

// ReceiveData test moved to later in the file to keep receive-related tests grouped together.

TEST(LoraTransportTest, GetSessionLifecycle)
{
    auto Radio   = std::make_unique<NiceMock<MockRadio>>();
    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));

    ASSERT_NE(Session, nullptr);
    auto Id    = Session->ID();
    auto Found = Manager->GetSession(Id);
    EXPECT_NE(Found, nullptr);
    EXPECT_EQ(Found.get(), Session.get());

    Manager->Close(*Session);
    auto FoundAfter = Manager->GetSession(Id);
    EXPECT_EQ(FoundAfter, nullptr);
    EXPECT_FALSE(Session->IsOpen());
}

TEST(LoraTransportTest, SessionIDs_AreUnique)
{
    auto Radio   = std::make_unique<NiceMock<MockRadio>>();
    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));

    auto S1 = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));
    auto S2 = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));

    ASSERT_NE(S1, nullptr);
    ASSERT_NE(S2, nullptr);

    EXPECT_NE(S1->ID(), S2->ID());
}

TEST(LoraTransportTest, CreateSession_NullConfigReturnsNull)
{
    auto Radio   = std::make_unique<NiceMock<MockRadio>>();
    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));

    std::unique_ptr<ITransferConfig> NullCfg(nullptr);
    auto Session = Manager->CreateSession(std::move(NullCfg));
    EXPECT_EQ(Session, nullptr);
}

TEST(LoraTransportTest, Send_RetriesUntilSuccess)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();

    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));

    std::vector<std::byte> Data = {std::byte {0x10}};

    // First send fails, second succeeds — ensure queue retains message on failure
    EXPECT_CALL(*RadioPtr, send(_, _)).WillOnce(Return(false)).WillOnce(Return(true));

    EXPECT_TRUE(Session->Send(Data));

    // First Process -> send returns false (message stays in queue)
    Manager->Process();
    // Second Process -> send returns true (message dequeued)
    Manager->Process();
}

TEST(LoraTransportTest, Send_EmptyData_AllowsEmptyPayload)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();

    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));

    std::vector<std::byte> Data = {};

    // Behavior: sending an empty payload is allowed and should result in at most one send invocation
    EXPECT_CALL(*RadioPtr, send(_, _)).Times(1).WillOnce(Return(true));

    EXPECT_TRUE(Session->Send(Data));
    Manager->Process();
}

TEST(LoraTransportTest, Receive_DispatchToAllSessions)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();

    auto Manager  = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session1 = Manager->CreateSession(std::make_unique<LoraTransferConfig>(1, nullptr));
    auto Session2 = Manager->CreateSession(std::make_unique<LoraTransferConfig>(2, nullptr));

    int Called1 = 0;
    int Called2 = 0;

    Session1->RegisterCallback(
        [&](const TransferContext&, std::span<const std::byte> payload)
        {
            Called1++;
            EXPECT_EQ(payload.size(), 3);
        });
    Session2->RegisterCallback(
        [&](const TransferContext&, std::span<const std::byte> payload)
        {
            Called2++;
            EXPECT_EQ(payload.size(), 3);
        });

    EXPECT_CALL(*RadioPtr, receive(_, _, _))
        .WillOnce(
            [](uint8_t* buffer, size_t maxLen, size_t& outLen)
            {
                if (maxLen < 3) { return false; }
                buffer[0] = 0x11;
                buffer[1] = 0x22;
                buffer[2] = 0x33;
                outLen    = 3;
                return true;
            })
        .WillRepeatedly(Return(false));

    Manager->Process();

    EXPECT_EQ(Called1, 1);
    EXPECT_EQ(Called2, 1);
}

TEST(LoraTransportTest, SendQueue_Full_RejectsExcess)
{
    auto Radio   = std::make_unique<NiceMock<MockRadio>>();
    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));

    std::vector<std::byte> Data = {std::byte {0x55}};

    // Enqueue until the API indicates the queue is full; don't rely on internal capacity constants
    size_t successCount = 0;
    while (Session->Send(Data))
    {
        ++successCount;
        // safety in case of a bug causing infinite loop
        if (successCount > 10000) { break; }
    }

    EXPECT_GT(successCount, 0u);
    EXPECT_FALSE(Session->Send(Data));
}

TEST(LoraTransportTest, ReceiveData)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();

    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));

    bool CallbackCalled = false;
    Session->RegisterCallback(
        [&](const TransferContext&, std::span<const std::byte> payload)
        {
            CallbackCalled = true;
            EXPECT_EQ(payload.size(), 2);
            EXPECT_EQ(payload[0], std::byte {0xAA});
            EXPECT_EQ(payload[1], std::byte {0xBB});
        });

    EXPECT_CALL(*RadioPtr, receive(_, _, _))
        .WillOnce(
            [](uint8_t* buffer, size_t maxLen, size_t& outLen)
            {
                if (maxLen < 2) { return false; }
                buffer[0] = 0xAA;
                buffer[1] = 0xBB;
                outLen    = 2;
                return true;
            })
        .WillRepeatedly(Return(false));

    Manager->Process();

    EXPECT_TRUE(CallbackCalled);
}

// Removed: ManagerScope_ExpiresSessionsOnScopeExit (duplicate of ManagerDestruction_ExpiresSessions)
// Duplicate test removed to keep tests focused on the public API.

TEST(LoraTransportTest, SessionScope_ManagerKeepsSessionAlive)
{
    auto Radio   = std::make_unique<NiceMock<MockRadio>>();
    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    size_t id    = 0;
    {
        auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(5, nullptr));
        ASSERT_NE(Session, nullptr);
        id = Session->ID();
    }

    // Even after the local shared_ptr goes out of scope, manager still owns the session
    auto Found = Manager->GetSession(id);
    ASSERT_NE(Found, nullptr);
    EXPECT_TRUE(Found->IsOpen());
}

// Removed: BothScoped_ExpireAfterScope
// This scenario is adequately covered by other lifetime tests (e.g. ManagerDestruction_ExpiresSessions
// and SessionOutlivesManager_SendsThenFailsAfterScope). Keeping tests focused on API behavior.

TEST(LoraTransportTest, SessionOutlivesManager_SendsThenFailsAfterScope)
{
    std::shared_ptr<ITransferSession> Session;

    // Manager will be scoped inside this block and destroyed at block exit
    {
        auto Radio   = std::make_unique<NiceMock<MockRadio>>();
        auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));

        Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(3, nullptr));
        ASSERT_NE(Session, nullptr);
        EXPECT_TRUE(Session->IsOpen());

        std::vector<std::byte> Data = {std::byte {0xDE}};
        // While manager is alive, Send should succeed (queues message)
        EXPECT_TRUE(Session->Send(Data));
    }

    // Manager has gone out of scope -> session should observe expired manager
    EXPECT_FALSE(Session->IsOpen());
    std::vector<std::byte> Data2 = {std::byte {0xAD}};
    EXPECT_FALSE(Session->Send(Data2));
}

TEST(LoraTransportTest, SessionScopedButManagerLives_CanRetrieveAndSend)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();
    auto Manager   = std::make_shared<LoraTransferManager>(std::move(Radio));

    size_t sessionId = 0;
    {
        auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(7, nullptr));
        ASSERT_NE(Session, nullptr);
        sessionId = Session->ID();

        std::vector<std::byte> Data = {std::byte {0x01}};
        EXPECT_TRUE(Session->Send(Data));

        // ensure queued message is processed while manager is still alive
        EXPECT_CALL(*RadioPtr, send(_, _)).Times(1).WillOnce(Return(true));
        Manager->Process();
    }

    // original local Session has gone out of scope, but manager still owns it
    auto Found = Manager->GetSession(sessionId);
    ASSERT_NE(Found, nullptr);
    EXPECT_TRUE(Found->IsOpen());

    std::vector<std::byte> Data2 = {std::byte {0x02}};
    EXPECT_TRUE(Found->Send(Data2));
    EXPECT_CALL(*RadioPtr, send(_, _)).Times(1).WillOnce(Return(true));
    Manager->Process();
}

TEST(LoraTransportTest, ManagerDestruction_ExpiresSessions)
{
    auto Radio   = std::make_unique<NiceMock<MockRadio>>();
    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));

    ASSERT_NE(Session, nullptr);
    EXPECT_TRUE(Session->IsOpen());

    // Destroy manager -> session should observe manager weak_ptr expired
    Manager.reset();

    EXPECT_FALSE(Session->IsOpen());

    std::vector<std::byte> Data = {std::byte {0x99}};
    // Send should fail because manager expired
    EXPECT_FALSE(Session->Send(Data));
}

TEST(LoraTransportTest, SessionConfigHasValues)
{
    int SomeCtx  = 123;
    auto Radio   = std::make_unique<NiceMock<MockRadio>>();
    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));

    auto Config  = std::make_unique<LoraTransferConfig>(55, &SomeCtx);
    auto Session = Manager->CreateSession(std::move(Config));
    ASSERT_NE(Session, nullptr);

    const auto& Cfg = static_cast<const LoraTransferConfig&>(Session->Config());
    EXPECT_EQ(Cfg.PeerId, 55);
    EXPECT_EQ(Cfg.Ctx, &SomeCtx);
}

TEST(LoraTransportTest, Close_ReturnsFalseOnSecondClose)
{
    auto Radio   = std::make_unique<NiceMock<MockRadio>>();
    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));

    ASSERT_NE(Session, nullptr);
    EXPECT_TRUE(Manager->Close(*Session));
    EXPECT_FALSE(Manager->Close(*Session));
    EXPECT_FALSE(Session->IsOpen());
}

TEST(LoraTransportTest, GetSession_InvalidID_ReturnsNull)
{
    auto Radio   = std::make_unique<NiceMock<MockRadio>>();
    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));

    auto Found = Manager->GetSession(0xFFFFFFFF);
    EXPECT_EQ(Found, nullptr);
}

TEST(LoraTransportTest, SendFailsAfterCloseAndNoRadioActivity)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();
    auto Manager   = std::make_shared<LoraTransferManager>(std::move(Radio));

    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));
    ASSERT_NE(Session, nullptr);

    // Close the session and ensure further sends fail and no radio send occurs
    EXPECT_TRUE(Manager->Close(*Session));
    EXPECT_FALSE(Session->IsOpen());

    std::vector<std::byte> Data = {std::byte {0xAA}};
    EXPECT_FALSE(Session->Send(Data));

    // No radio activity should happen when processing with no active sessions
    EXPECT_CALL(*RadioPtr, send(_, _)).Times(0);
    Manager->Process();
}

TEST(LoraTransportTest, Close_OnDifferentManagerReturnsFalse)
{
    auto Radio1   = std::make_unique<NiceMock<MockRadio>>();
    auto Manager1 = std::make_shared<LoraTransferManager>(std::move(Radio1));
    auto Session  = Manager1->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));
    ASSERT_NE(Session, nullptr);

    auto Radio2   = std::make_unique<NiceMock<MockRadio>>();
    auto Manager2 = std::make_shared<LoraTransferManager>(std::move(Radio2));

    // Attempting to close a session with a different manager should fail
    EXPECT_FALSE(Manager2->Close(*Session));
}

TEST(LoraTransportTest, CallbackContextContainsSessionAndType)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();
    auto Manager   = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session   = Manager->CreateSession(std::make_unique<LoraTransferConfig>(9, nullptr));
    ASSERT_NE(Session, nullptr);

    bool CallbackCalled = false;
    Session->RegisterCallback(
        [&](const TransferContext& Ctx, std::span<const std::byte> payload)
        {
            CallbackCalled = true;
            EXPECT_EQ(Ctx.Type, TransportType::Lora);
            ASSERT_NE(Ctx.Session, nullptr);
            EXPECT_EQ(Ctx.Session->ID(), Session->ID());
        });

    EXPECT_CALL(*RadioPtr, receive(_, _, _))
        .WillOnce(
            [](uint8_t* buffer, size_t maxLen, size_t& outLen)
            {
                if (maxLen < 1) { return false; }
                buffer[0] = 0x05;
                outLen    = 1;
                return true;
            })
        .WillRepeatedly(Return(false));

    Manager->Process();
    EXPECT_TRUE(CallbackCalled);
}

TEST(LoraTransportTest, ManagerSendDirectlyQueuesAndSends)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();
    auto Manager   = std::make_shared<LoraTransferManager>(std::move(Radio));

    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));
    ASSERT_NE(Session, nullptr);

    std::vector<std::byte> Data = {std::byte {0x01}};
    EXPECT_TRUE(Session->Send(Data));

    EXPECT_CALL(*RadioPtr, send(_, _)).Times(1).WillOnce(Return(true));
    Manager->Process();
}

TEST(LoraTransportTest, MultipleManagers_IsolatedRadios)
{
    auto Radio1   = std::make_unique<NiceMock<MockRadio>>();
    auto* R1      = Radio1.get();
    auto Manager1 = std::make_shared<LoraTransferManager>(std::move(Radio1));

    auto Radio2   = std::make_unique<NiceMock<MockRadio>>();
    auto* R2      = Radio2.get();
    auto Manager2 = std::make_shared<LoraTransferManager>(std::move(Radio2));

    auto Session1 = Manager1->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));
    auto Session2 = Manager2->CreateSession(std::make_unique<LoraTransferConfig>(0, nullptr));
    ASSERT_NE(Session1, nullptr);
    ASSERT_NE(Session2, nullptr);

    std::vector<std::byte> Data = {std::byte {0x02}};
    EXPECT_TRUE(Session1->Send(Data));
    EXPECT_TRUE(Session2->Send(Data));

    EXPECT_CALL(*R1, send(_, _)).Times(1).WillOnce(Return(true));
    EXPECT_CALL(*R2, send(_, _)).Times(1).WillOnce(Return(true));

    Manager1->Process();
    Manager2->Process();
}

TEST(LoraTransportTest, CallbackReplacement_UsesLatest)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();
    auto Manager   = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session   = Manager->CreateSession(std::make_unique<LoraTransferConfig>(1, nullptr));
    ASSERT_NE(Session, nullptr);

    bool firstCalled  = false;
    bool secondCalled = false;

    Session->RegisterCallback([&](const TransferContext&, std::span<const std::byte>) { firstCalled = true; });
    Session->RegisterCallback([&](const TransferContext&, std::span<const std::byte>) { secondCalled = true; });

    EXPECT_CALL(*RadioPtr, receive(_, _, _))
        .WillOnce(
            [](uint8_t* buffer, size_t maxLen, size_t& outLen)
            {
                if (maxLen < 1) { return false; }
                buffer[0] = 0x09;
                outLen    = 1;
                return true;
            })
        .WillRepeatedly(Return(false));

    Manager->Process();

    EXPECT_FALSE(firstCalled);
    EXPECT_TRUE(secondCalled);
}

TEST(LoraTransportTest, ClosedSessionDoesNotReceive)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();

    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto S1      = Manager->CreateSession(std::make_unique<LoraTransferConfig>(1, nullptr));
    auto S2      = Manager->CreateSession(std::make_unique<LoraTransferConfig>(2, nullptr));

    int Called1 = 0;
    int Called2 = 0;

    S1->RegisterCallback([&](const TransferContext&, std::span<const std::byte> payload) { Called1++; });
    S2->RegisterCallback([&](const TransferContext&, std::span<const std::byte> payload) { Called2++; });

    // Close S1 — it should not receive data anymore
    Manager->Close(*S1);

    EXPECT_CALL(*RadioPtr, receive(_, _, _))
        .WillOnce(
            [](uint8_t* buffer, size_t maxLen, size_t& outLen)
            {
                if (maxLen < 1) { return false; }
                buffer[0] = 0x7F;
                outLen    = 1;
                return true;
            })
        .WillRepeatedly(Return(false));

    Manager->Process();

    EXPECT_EQ(Called1, 0);
    EXPECT_EQ(Called2, 1);
}
