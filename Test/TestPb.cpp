#include <gtest/gtest.h>
#include <array>

#include "ProtoCodec.h"
#include "Random.h"

auto& RNG = ra::RNG::Instance();

TEST(ProtoEncodeTest, ProtoEncodeSingle)
{
    const uint32_t severity = RNG.Value<uint32_t>();
    std::string test_str    = RNG.String(RNG.Value<uint8_t>());

    // Encode the message
    const auto data          = ra::turtleford::PbGen_DebugMsg(severity, &test_str);
    const auto bytes_written = ra::turtleford::ProtoEncode(data).size();

    // Check if the function wrote some bytes (non-zero result).
    ASSERT_GT(bytes_written, 0);
}
TEST(ProtoEncodeTest, ProtoEncodeSingleBufferSmall)
{
    const auto status    = RNG.Value<uint32_t>();
    std::string test_str = RNG.String(RNG.Value<uint8_t>());

    const auto data            = ra::turtleford::PbGen_DebugMsg(status, &test_str);
    const auto required_length = ra::turtleford::ProtoEncode(data, {});

    std::array<std::byte, 1> buffer;
    // Encode the message
    const uint32_t bytes_written = ra::turtleford::ProtoEncode(data, buffer);

    // Check if the function wrote the exact bytes
    ASSERT_GT(required_length, 0);
    ASSERT_EQ(bytes_written, 0);
}

TEST(ProtoEncodeTest, ProtoEncodeLogMessage)
{
    const auto status        = RNG.Value<uint32_t>();
    const uint32_t timestamp = RNG.Value<uint32_t>();
    const uint32_t severity  = RNG.Value<uint32_t>();
    std::string test_str     = RNG.String(RNG.Value<uint8_t>());

    const auto data          = ra::turtleford::PbGen_DebugMsg(status, &test_str);
    const auto bytes_written = ra::turtleford::ProtoEncode(timestamp, severity, data).size();

    ASSERT_GT(bytes_written, 0);
}

TEST(ProtoDecodeTest, ProtoDecodeMainMessage)
{
    std::string test_str = RNG.String(RNG.Value<uint8_t>());
    const auto status    = RNG.Value<uint32_t>();
    const auto data      = ra::turtleford::PbGen_DebugMsg(status, &test_str);

    const auto encoded_data = ra::turtleford::ProtoEncode(data);

    // Decode the message
    const Proto_MainMessage decoded_msg = ra::turtleford::ProtoDecode_MainMessage(encoded_data);
    const auto& s                       = *static_cast<std::string*>(decoded_msg.message_type.debug_msg.msg.arg);

    // Verify that the decoded message contains the expected data
    ASSERT_EQ(decoded_msg.which_message_type, Proto_MainMessage_debug_msg_tag);
    ASSERT_EQ(decoded_msg.message_type.debug_msg.status, status);
    ASSERT_EQ(s, test_str);
}
TEST(ProtoDecodeTest, ProtoDecodeLogMessage)
{
    std::string test_str = RNG.String(RNG.Value<uint8_t>());

    const auto status        = RNG.Value<uint32_t>();
    const uint32_t timestamp = RNG.Value<uint32_t>();
    const uint32_t severity  = RNG.Value<uint32_t>();

    const auto data         = ra::turtleford::PbGen_DebugMsg(status, &test_str);
    const auto encoded_data = ra::turtleford::ProtoEncode(timestamp, severity, data);

    // Decode the LogMessage
    const Proto_LogMessage decoded_log_msg = ra::turtleford::ProtoDecode_LogMessage(encoded_data);

    const auto& s = *static_cast<std::string*>(decoded_log_msg.main_message.message_type.debug_msg.msg.arg);

    // Verify if the LogMessage is correctly decoded
    ASSERT_EQ(decoded_log_msg.time_stamp, timestamp);
    ASSERT_EQ(decoded_log_msg.severity, severity);
    ASSERT_EQ(decoded_log_msg.main_message.which_message_type, Proto_MainMessage_debug_msg_tag);
    ASSERT_EQ(decoded_log_msg.main_message.message_type.debug_msg.status, status);
    ASSERT_EQ(s, test_str);
}

TEST(ProtoDecodeTest, ProtoDecodeFailure)
{
    std::array<std::byte, 1024> buffer;

    const Proto_MainMessage decoded_msg = ra::turtleford::ProtoDecode_MainMessage(buffer);
    // Assert that decoding fails and returns an empty message (or handles the error gracefully).
    ASSERT_EQ(decoded_msg.which_message_type, 0); // No valid message type
}
