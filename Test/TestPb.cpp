#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "ProtoCodec.h"
#include "Random.h"

using testing::AllOf;
using testing::ElementsAreArray;
using testing::Field;
using testing::IsEmpty;
using testing::Not;
using testing::Optional;
using testing::ResultOf;
using testing::_;

namespace
{
ra::RNG RNG;

// The decoded debug message of a main message
auto DebugStatus(uint32_t Status)
{
    return ResultOf("debug status", [](const Proto_MainMessage& M) { return M.message_type.debug.status; }, Status);
}

auto DebugText(const std::string& Text)
{
    return ResultOf(
        "debug text", [](const Proto_MainMessage& M) { return std::string {M.message_type.debug.msg}; }, Text);
}

// A decoded log message: how severe, what kind, when, and the debug message inside
auto IsLogOf(uint32_t Timestamp, uint32_t Severity, uint32_t Category, uint32_t Status, const std::string& Text)
{
    return AllOf(Field("severity", &Proto_LogMessage::severity, Severity),
                 Field("category", &Proto_LogMessage::category, Category),
                 Field("main_message",
                       &Proto_LogMessage::main_message,
                       AllOf(Field("timestamp", &Proto_MainMessage::timestamp, Timestamp),
                             Field("which_message_type",
                                   &Proto_MainMessage::which_message_type,
                                   Proto_MainMessage_debug_tag),
                             DebugStatus(Status),
                             DebugText(Text))));
}

std::vector<std::byte> BytesOf(std::span<const std::byte> Data) { return {Data.begin(), Data.end()}; }
} // namespace

namespace
{
std::vector<std::byte> EncodeMain(const Proto_MainMessage& Message,
                                  uint32_t TimeStamp               = 0u,
                                  ra::turtleford::ProtoFlags Flags = ra::turtleford::ProtoFlags::None)
{
    const auto Size = ra::turtleford::ProtoEncode(TimeStamp, Message, {}, Flags);
    std::vector<std::byte> Buffer(Size);
    if (Size > 0) { ra::turtleford::ProtoEncode(TimeStamp, Message, Buffer, Flags); }
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
    std::string TestStr   = RNG.String(RNG.Value<uint8_t>(0, ra::turtleford::MaxDebugTextLength));

    // Encode the message
    const auto Data = ra::turtleford::PbGen_DebugMsg(Status, TestStr);

    // Check if the function wrote some bytes (non-zero result).
    ASSERT_THAT(EncodeMain(Data), Not(IsEmpty()));
}
TEST(ProtoEncodeTest, ProtoEncodeSingleBufferSmall)
{
    const auto Status   = RNG.Value<uint32_t>();
    std::string TestStr = RNG.String(RNG.Value<uint8_t>(0, ra::turtleford::MaxDebugTextLength));

    const auto Data           = ra::turtleford::PbGen_DebugMsg(Status, TestStr);
    const auto RequiredLength = ra::turtleford::ProtoEncode(0u, Data, {});

    std::array<std::byte, 1> Buffer;
    // Encode the message
    const auto BytesWritten = ra::turtleford::ProtoEncode(0u, Data, Buffer);

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
    std::string TestStr      = RNG.String(RNG.Value<uint8_t>(0, ra::turtleford::MaxDebugTextLength));

    const auto Data = ra::turtleford::PbGen_DebugMsg(Status, TestStr);

    ASSERT_THAT(EncodeLog(Timestamp, Severity, Category, Data), Not(IsEmpty()));
}

TEST(ProtoFrameTest, ProtoFrameLogMessageRoundTrip)
{
    const auto Status        = RNG.Value<uint32_t>();
    const uint32_t Timestamp = RNG.Value<uint32_t>();
    const uint32_t Severity  = RNG.Value<uint32_t>();
    const auto Category      = ra::type::Category::Application;
    std::string TestStr      = RNG.String(RNG.Value<uint8_t>(0, ra::turtleford::MaxDebugTextLength));

    const auto Data   = ra::turtleford::PbGen_DebugMsg(Status, TestStr);
    const auto Framed = EncodeLog(Timestamp, Severity, Category, Data, ra::turtleford::ProtoFlags::Framed);

    ASSERT_THAT(ra::turtleford::ProtoFrame_Read(Framed),
                Optional(Field("BytesConsumed", &ra::turtleford::ProtoFrame::BytesConsumed, Framed.size())));

    const auto Decode = ra::turtleford::ProtoDecode_LogMessage(Framed, ra::turtleford::ProtoFlags::Framed);
    ASSERT_THAT(Decode, Optional(IsLogOf(Timestamp, Severity, static_cast<uint32_t>(Category), Status, TestStr)));
}

TEST(ProtoFrameTest, ProtoFrameWrapsExistingEncodedPayload)
{
    const auto TestString = RNG.String(RNG.Value<uint8_t>(0, ra::turtleford::MaxDebugTextLength));
    const auto Message    = ra::turtleford::PbGen_DebugMsg(RNG.Value<uint32_t>(), TestString);
    const auto Encoded    = EncodeLog(10u, 20u, ra::type::Category::Application, Message);
    const auto Framed     = WriteFrame(Encoded);

    const auto Frame = ra::turtleford::ProtoFrame_Read(Framed);
    ASSERT_THAT(Frame, Optional(Field("BytesConsumed", &ra::turtleford::ProtoFrame::BytesConsumed, Framed.size())));
    ASSERT_THAT(BytesOf(Frame->Payload), ElementsAreArray(Encoded));
}

TEST(ProtoFrameTest, ProtoFrameRejectsTruncatedMessage)
{
    const auto TestString = RNG.String(RNG.Value<uint8_t>(0, ra::turtleford::MaxDebugTextLength));
    const auto Data       = ra::turtleford::PbGen_DebugMsg(RNG.Value<uint32_t>(), TestString);
    auto Framed = EncodeLog(1u, 2u, ra::type::Category::Application, Data, ra::turtleford::ProtoFlags::Framed);
    ASSERT_GT(Framed.size(), 1u);

    Framed.pop_back();

    ASSERT_THAT(ra::turtleford::ProtoFrame_Read(Framed), Not(Optional(_)));
    ASSERT_THAT(ra::turtleford::ProtoDecode_LogMessage(Framed, ra::turtleford::ProtoFlags::Framed), Not(Optional(_)));
}

TEST(ProtoDecodeTest, ProtoDecodeMainMessage)
{
    const std::string TestStr = RNG.String(RNG.Value<uint8_t>(0, ra::turtleford::MaxDebugTextLength));
    const auto Status         = RNG.Value<uint32_t>();
    const auto Data           = ra::turtleford::PbGen_DebugMsg(Status, TestStr);

    const auto EncodedData = EncodeMain(Data);

    // Decode the message
    const auto Decode = ra::turtleford::ProtoDecode_MainMessage(EncodedData);

    // Verify that the decoded message contains the expected data
    ASSERT_THAT(Decode,
                Optional(AllOf(Field("which_message_type",
                                     &Proto_MainMessage::which_message_type,
                                     Proto_MainMessage_debug_tag),
                               DebugStatus(Status),
                               DebugText(TestStr))));
}

TEST(ProtoDecodeTest, ProtoDecodeLogMessage)
{
    std::string TestStr = RNG.String(RNG.Value<uint8_t>(0, ra::turtleford::MaxDebugTextLength));

    const auto Status        = RNG.Value<uint32_t>();
    const uint32_t Timestamp = RNG.Value<uint32_t>();
    const uint32_t Severity  = RNG.Value<uint32_t>();
    const auto Category      = ra::type::Category::Application;

    const auto Data        = ra::turtleford::PbGen_DebugMsg(Status, TestStr);
    const auto EncodedData = EncodeLog(Timestamp, Severity, Category, Data);

    // Decode the LogMessage
    const auto Decode = ra::turtleford::ProtoDecode_LogMessage(EncodedData);

    // Verify if the LogMessage is correctly decoded
    ASSERT_THAT(Decode, Optional(IsLogOf(Timestamp, Severity, static_cast<uint32_t>(Category), Status, TestStr)));
}

// Protobuf has no checksum, so the decoder cannot tell random bytes from a message: an empty buffer, or bytes that
// happen to form fields this message does not have (they are skipped), decode fine. Only input that breaks the wire
// format is refused. Radio drops a decoded frame without an epoch (RadioTest.FrameThatDecodesButHasNoEpochIsIgnored).
namespace
{
std::vector<std::byte> Bytes(std::initializer_list<uint8_t> Values)
{
    std::vector<std::byte> Out;
    for (const uint8_t V : Values) { Out.push_back(std::byte {V}); }
    return Out;
}
} // namespace

TEST(ProtoDecodeTest, InputThatBreaksTheWireFormatIsRefused)
{
    const std::pair<const char*, std::vector<std::byte>> Cases[] = {
        {"tag cut off", Bytes({0x80})},
        {"varint value cut off", Bytes({0x08, 0x80})},
        {"length past the end", Bytes({0x0A, 0x05, 0x01})},
        {"fixed32 cut off", Bytes({0x0D, 0x01})},
        {"fixed64 cut off", Bytes({0x09, 0x01, 0x02})},
        {"wire type 6", Bytes({0x0E, 0x00})},
        {"wire type 7", Bytes({0x0F, 0x00})},
    };
    for (const auto& [What, Buffer] : Cases)
    {
        EXPECT_THAT(ra::turtleford::ProtoDecode_MainMessage(Buffer), Not(Optional(_))) << What;
    }
}

TEST(ProtoDecodeTest, AMessageMissingItsLastByteIsRefused)
{
    // Whatever field comes last, losing its last byte leaves it incomplete
    auto Buffer = EncodeMain(ra::turtleford::PbGen_DebugMsg(7, "cut short"), 1234);
    ASSERT_GT(Buffer.size(), 1u);
    Buffer.pop_back();

    EXPECT_THAT(ra::turtleford::ProtoDecode_MainMessage(Buffer), Not(Optional(_)));
}

TEST(ProtoDecodeTest, EmptyOrUnknownFieldsDecodeToAMessageWithNoEpoch)
{
    // field 177549 (not in MainMessage), varint 58; and field 271, fixed64
    const std::vector<std::byte> Cases[] = {
        {},
        Bytes({0xE8, 0xD8, 0x56, 0x3A}),
        Bytes({0xF9, 0x10, 0x2E, 0x13, 0x8F, 0x09, 0xBB, 0xE4, 0x01, 0x02}),
    };
    for (const auto& Buffer : Cases)
    {
        const auto Decoded = ra::turtleford::ProtoDecode_MainMessage(Buffer);
        ASSERT_THAT(Decoded, Optional(_)) << testing::PrintToString(Buffer);
        EXPECT_EQ(Decoded->epoch, 0u);
        EXPECT_EQ(Decoded->which_message_type, 0u);
    }
}

TEST(ProtoDecodeTest, RandomBytesNeverCrashTheDecoder)
{
    // Fixed seed, so a failure can be reproduced. Whether each buffer decodes is not checked: see above.
    ra::RNG Fixed {20261123};
    for (int i = 0; i < 5000; ++i)
    {
        const auto Buffer = Fixed.ByteVector(Fixed.Value(0, 255));
        (void)ra::turtleford::ProtoDecode_MainMessage(Buffer);
        (void)ra::turtleford::ProtoDecode_MainMessage(Buffer, ra::turtleford::ProtoFlags::Framed);
    }
}
