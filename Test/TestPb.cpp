#include <gtest/gtest.h>
#include <array>

#include "ProtoCodec.h"
#include "Random.h"

auto& RNG = ra::RNG::Instance();

TEST(ProtoEncodeTest, ProtoEncodeSingle)
{
    const uint32_t Status = RNG.Value<uint32_t>();
    std::string TestStr   = RNG.String(RNG.Value<uint8_t>());

    // Encode the message
    const auto Data         = ra::turtleford::PbGen_DebugMsg(Status, TestStr);
    const auto BytesWritten = ra::turtleford::ProtoEncode(Data).size();

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
    const uint32_t Location  = RNG.Value<uint32_t>();
    std::string TestStr      = RNG.String(RNG.Value<uint8_t>());

    const auto Data         = ra::turtleford::PbGen_DebugMsg(Status, TestStr);
    const auto BytesWritten = ra::turtleford::ProtoEncode(Timestamp, Severity, Location, Data).size();

    ASSERT_GT(BytesWritten, 0);
}

TEST(ProtoDecodeTest, ProtoDecodeMainMessage)
{
    const std::string TestStr = RNG.String(RNG.Value<uint8_t>());
    const auto Status         = RNG.Value<uint32_t>();
    const auto Data           = ra::turtleford::PbGen_DebugMsg(Status, TestStr);

    const auto EncodedData = ra::turtleford::ProtoEncode(Data);

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
    const uint32_t Location  = RNG.Value<uint32_t>();

    const auto Data        = ra::turtleford::PbGen_DebugMsg(Status, TestStr);
    const auto EncodedData = ra::turtleford::ProtoEncode(Timestamp, Severity, Location, Data);

    // Decode the LogMessage
    const auto Decode = ra::turtleford::ProtoDecode_LogMessage(EncodedData);
    ASSERT_TRUE(Decode.has_value());
    const Proto_LogMessage decoded_log_msg = Decode.value();

    const auto MsgPtr = std::unique_ptr<std::string>(
        static_cast<std::string*>(decoded_log_msg.main_message.message_type.debug_msg.msg.arg));

    // Verify if the LogMessage is correctly decoded
    ASSERT_EQ(decoded_log_msg.time_stamp, Timestamp);
    ASSERT_EQ(decoded_log_msg.severity, Severity);
    ASSERT_EQ(decoded_log_msg.location, Location);
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
