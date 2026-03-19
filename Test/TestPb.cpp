#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <vector>

#include "ProtoCodec.h"
#include "Random.h"

auto& RNG = ra::RNG::Instance();

namespace
{
std::vector<std::byte> EncodeMain(const Proto_MainMessage& Message,
                                  ra::turtleford::ProtoFlags Flags = ra::turtleford::ProtoFlags::None)
{
    const auto Size = ra::turtleford::ProtoEncode(Message, {}, Flags);
    std::vector<std::byte> Buffer(Size);
    if (Size > 0) { ra::turtleford::ProtoEncode(Message, Buffer, Flags); }
    return Buffer;
}

std::vector<std::byte> EncodeLog(uint32_t Timestamp,
                                 uint32_t Severity,
                                 ra::type::Category Category,
                                 const Proto_MainMessage& Message,
                                 ra::turtleford::ProtoFlags Flags = ra::turtleford::ProtoFlags::None)
{
    const auto Size = ra::turtleford::ProtoEncode(Timestamp, Severity, Category, Message, {}, Flags);
    std::vector<std::byte> Buffer(Size);
    if (Size > 0) { ra::turtleford::ProtoEncode(Timestamp, Severity, Category, Message, Buffer, Flags); }
    return Buffer;
}

std::vector<std::byte> WriteFrame(std::span<const std::byte> Payload)
{
    const auto Size = ra::turtleford::ProtoFrame_Write(Payload, {});
    std::vector<std::byte> Buffer(Size);
    if (Size > 0) { ra::turtleford::ProtoFrame_Write(Payload, Buffer); }
    return Buffer;
}
} // namespace

TEST(ProtoEncodeTest, ProtoEncodeSingle)
{
    const uint32_t Status = RNG.Value<uint32_t>();
    std::string TestStr   = RNG.String(RNG.Value<uint8_t>());

    // Encode the message
    const auto Data         = ra::turtleford::PbGen_DebugMsg(Status, TestStr);
    const auto BytesWritten = EncodeMain(Data).size();

    // Check if the function wrote some bytes (non-zero result).
    ASSERT_GT(BytesWritten, 0);
}
TEST(ProtoEncodeTest, ProtoEncodeSingleBufferSmall)
{
    const auto Status   = RNG.Value<uint32_t>();
    std::string TestStr = RNG.String(RNG.Value<uint8_t>());

    const auto Data           = ra::turtleford::PbGen_DebugMsg(Status, TestStr);
    const auto RequiredLength = ra::turtleford::ProtoEncode(Data, {});

    std::array<std::byte, 1> Buffer;
    // Encode the message
    const auto BytesWritten = ra::turtleford::ProtoEncode(Data, Buffer);

    // Check if the function wrote the exact bytes
    ASSERT_GT(RequiredLength, 0);
    ASSERT_EQ(BytesWritten, 0);
}

TEST(ProtoEncodeTest, ProtoEncodeLogMessage)
{
    const auto Status        = RNG.Value<uint32_t>();
    const uint32_t Timestamp = RNG.Value<uint32_t>();
    const uint32_t Severity  = RNG.Value<uint32_t>();
    const auto Category      = ra::type::Category::Application;
    std::string TestStr      = RNG.String(RNG.Value<uint8_t>());

    const auto Data         = ra::turtleford::PbGen_DebugMsg(Status, TestStr);
    const auto BytesWritten = EncodeLog(Timestamp, Severity, Category, Data).size();

    ASSERT_GT(BytesWritten, 0);
}

TEST(ProtoFrameTest, ProtoFrameLogMessageRoundTrip)
{
    const auto Status        = RNG.Value<uint32_t>();
    const uint32_t Timestamp = RNG.Value<uint32_t>();
    const uint32_t Severity  = RNG.Value<uint32_t>();
    const auto Category      = ra::type::Category::Application;
    std::string TestStr      = RNG.String(RNG.Value<uint8_t>());

    const auto Data   = ra::turtleford::PbGen_DebugMsg(Status, TestStr);
    const auto Framed = EncodeLog(Timestamp, Severity, Category, Data, ra::turtleford::ProtoFlags::Framed);

    const auto Frame = ra::turtleford::ProtoFrame_Read(Framed);
    ASSERT_TRUE(Frame.has_value());
    ASSERT_EQ(Frame->BytesConsumed, Framed.size());

    const auto Decode = ra::turtleford::ProtoDecode_LogMessage(Framed, ra::turtleford::ProtoFlags::Framed);
    ASSERT_TRUE(Decode.has_value());

    auto MsgPtr =
        std::unique_ptr<std::string>(static_cast<std::string*>(Decode->main_message.message_type.debug_msg.msg.arg));

    ASSERT_EQ(Decode->time_stamp, Timestamp);
    ASSERT_EQ(Decode->severity, Severity);
    ASSERT_EQ(Decode->category, static_cast<uint32_t>(Category));
    ASSERT_EQ(Decode->main_message.message_type.debug_msg.status, Status);
    ASSERT_EQ(*MsgPtr, TestStr);
}

TEST(ProtoFrameTest, ProtoFrameWrapsExistingEncodedPayload)
{
    const auto Message = ra::turtleford::PbGen_DebugMsg(42u, "wrapped");
    const auto Encoded = EncodeLog(10u, 20u, ra::type::Category::Application, Message);
    const auto Framed  = WriteFrame(Encoded);

    const auto Frame = ra::turtleford::ProtoFrame_Read(Framed);
    ASSERT_TRUE(Frame.has_value());
    ASSERT_EQ(Frame->BytesConsumed, Framed.size());
    ASSERT_EQ(Frame->Payload.size(), Encoded.size());
    ASSERT_TRUE(std::equal(Frame->Payload.begin(), Frame->Payload.end(), Encoded.begin(), Encoded.end()));
}

TEST(ProtoFrameTest, ProtoFrameRejectsTruncatedMessage)
{
    const auto Data = ra::turtleford::PbGen_DebugMsg(7u, "frame-test");
    auto Framed     = EncodeLog(1u, 2u, ra::type::Category::Application, Data, ra::turtleford::ProtoFlags::Framed);
    ASSERT_GT(Framed.size(), 1u);

    Framed.pop_back();

    ASSERT_FALSE(ra::turtleford::ProtoFrame_Read(Framed).has_value());
    ASSERT_FALSE(ra::turtleford::ProtoDecode_LogMessage(Framed, ra::turtleford::ProtoFlags::Framed).has_value());
}

TEST(ProtoDecodeTest, ProtoDecodeMainMessage)
{
    const std::string TestStr = RNG.String(RNG.Value<uint8_t>());
    const auto Status         = RNG.Value<uint32_t>();
    const auto Data           = ra::turtleford::PbGen_DebugMsg(Status, TestStr);

    const auto EncodedData = EncodeMain(Data);

    // Decode the message
    const auto Decode = ra::turtleford::ProtoDecode_MainMessage(EncodedData);
    ASSERT_TRUE(Decode.has_value());
    const Proto_MainMessage decoded_msg = Decode.value();
    const auto MsgPtr =
        std::unique_ptr<std::string>(static_cast<std::string*>(decoded_msg.message_type.debug_msg.msg.arg));

    // Verify that the decoded message contains the expected data
    ASSERT_EQ(decoded_msg.which_message_type, Proto_MainMessage_debug_msg_tag);
    ASSERT_EQ(decoded_msg.message_type.debug_msg.status, Status);
    ASSERT_EQ(*MsgPtr, TestStr);
}

TEST(ProtoDecodeTest, ProtoDecodeLogMessage)
{
    std::string TestStr = RNG.String(RNG.Value<uint8_t>());

    const auto Status        = RNG.Value<uint32_t>();
    const uint32_t Timestamp = RNG.Value<uint32_t>();
    const uint32_t Severity  = RNG.Value<uint32_t>();
    const auto Category      = ra::type::Category::Application;

    const auto Data        = ra::turtleford::PbGen_DebugMsg(Status, TestStr);
    const auto EncodedData = EncodeLog(Timestamp, Severity, Category, Data);

    // Decode the LogMessage
    const auto Decode = ra::turtleford::ProtoDecode_LogMessage(EncodedData);
    ASSERT_TRUE(Decode.has_value());
    const Proto_LogMessage decoded_log_msg = Decode.value();

    const auto MsgPtr = std::unique_ptr<std::string>(
        static_cast<std::string*>(decoded_log_msg.main_message.message_type.debug_msg.msg.arg));

    // Verify if the LogMessage is correctly decoded
    ASSERT_EQ(decoded_log_msg.time_stamp, Timestamp);
    ASSERT_EQ(decoded_log_msg.severity, Severity);
    ASSERT_EQ(decoded_log_msg.category, static_cast<uint32_t>(Category));
    ASSERT_EQ(decoded_log_msg.main_message.which_message_type, Proto_MainMessage_debug_msg_tag);
    ASSERT_EQ(decoded_log_msg.main_message.message_type.debug_msg.status, Status);
    ASSERT_EQ(*MsgPtr, TestStr);
}

TEST(ProtoDecodeTest, ProtoDecodeFailure)
{
    const auto Buffer = RNG.ByteVector(RNG.Value(0, 255));

    const auto Decode = ra::turtleford::ProtoDecode_MainMessage(Buffer);
    ASSERT_FALSE(Decode.has_value());
}
