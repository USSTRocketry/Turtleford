#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "Transport/LoraTransport.h"
#include "abstractions/ITelemetryRadio.h"
#include "RadioCmdReciever.h"
#include <memory>
#include <vector>
#include "ProtoCodec.h"

using namespace ra::turtleford;
using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

class MockRadio : public HAL::ITelemetryRadio
{
public:
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
};

TEST(RadioCommandRecieverTest, ConstructReceiver)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    // auto* RadioPtr = Radio.get();

    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(10, 20));

    RadioCmndReciever receiver(Session);

    SUCCEED();
}

TEST(RadioCommandRecieverTest, SendCommandCallsRadioSend)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();
    auto* RadioPtr = Radio.get();

    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(10, 20));

    RadioCmndReciever receiver(Session);

    Proto_MainMessage msg = {};

    EXPECT_CALL(*RadioPtr, send(_))
        .Times(1)
        .WillOnce(Return(true));

    receiver.SendCmnd(msg);

    Manager->Process();
}

TEST(RadioCommandRecieverTest, ReceiveCommandProcessesData)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();

    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(10, 20));

    RadioCmndReciever receiver(Session);

    TransferContext context{};

    std::vector<std::byte> buffer(64);

    std::span<const std::byte> data(buffer.data(), buffer.size());

    EXPECT_NO_THROW(
        receiver.RecieveCommand(context, data)
    );
}

TEST(RadioCommandRecieverTest, ReceiveFlightDataCallbackCalled)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();

    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(10, 20));

    RadioCmndReciever receiver(Session);

    bool called = false;

    receiver.RecieveData = [&](Proto_InFlightData)
    {
        called = true;
    };

    TransferContext context{};
    const type::FlightData data = {};
    std::vector<std::byte> buffer = ProtoEncode(PbGen_FlightData(data));

    receiver.RecieveCommand(context, buffer);

    EXPECT_TRUE(called);
}

TEST(RadioCommandRecieverTest, DebugMessageCallback)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();

    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(10, 20));

    RadioCmndReciever receiver(Session);

    bool called = false;

    receiver.DebugMessage = [&](std::unique_ptr<std::string>)
    {
        called = true;
    };

    TransferContext context{};
    std::vector<std::byte> buffer = ProtoEncode(PbGen_DebugMsg(0,std::string("Hello World")));

    receiver.RecieveCommand(context, buffer);

    EXPECT_TRUE(called);
}

TEST(RadioCommandRecieverTest, SwitchFrequencyCallback)
{
    auto Radio     = std::make_unique<NiceMock<MockRadio>>();

    auto Manager = std::make_shared<LoraTransferManager>(std::move(Radio));
    auto Session = Manager->CreateSession(std::make_unique<LoraTransferConfig>(10, 20));

    RadioCmndReciever receiver(Session);

    ra::hal::WorkQueue Work_queue;
    Work_queue.Init();
    receiver.SetWorkQueue(&Work_queue);

    float newFreq = 1000;

    receiver.SwitchRadioFrequency = [&](float freq)
    {
        newFreq = freq;
    };

    TransferContext context{};
    std::vector<std::byte> switchCommand = ProtoEncode(PbGen_SwitchFrequencyMsg(0.0));

    receiver.RecieveCommand(context, switchCommand);

    std::vector<std::byte> ackSwitch = ProtoEncode(PbGen_AckMsg(Proto_MainMessage_switch_radio_frequency_tag));

    receiver.RecieveCommand(context, ackSwitch);

    EXPECT_EQ(newFreq, 0.0);
}