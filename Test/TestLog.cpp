#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <span>
#include <vector>
#include <optional>

#include "Log.h"
#include "ProtoCodec.h"
#include "Random.h"
#include "Type.h"

using testing::AllOf;
using testing::Each;
using testing::ElementsAreArray;
using testing::Field;
using testing::IsEmpty;
using testing::Not;
using testing::Optional;
using testing::ResultOf;
using testing::SizeIs;
using testing::_;

static ra::RNG RNG;

namespace
{
// A chunk of the log storage that begins with exactly these bytes
MATCHER_P(StartsWithBytes, Prefix, "starts with the expected bytes")
{
    if (arg.size() < Prefix.size())
    {
        *result_listener << "is only " << arg.size() << " bytes";
        return false;
    }
    return std::equal(Prefix.begin(), Prefix.end(), arg.begin());
}

// A decoded log message with this severity and category, whose debug message has this status
auto IsLog(uint32_t Severity, ra::type::Category Category, uint32_t Status)
{
    return AllOf(Field("severity", &Proto_LogMessage::severity, Severity),
                 Field("category", &Proto_LogMessage::category, static_cast<uint32_t>(Category)),
                 ResultOf("debug status",
                          [](const Proto_LogMessage& M) { return M.main_message.message_type.debug.status; },
                          Status));
}

// A decoded log message that carries flight control, with or without a state
testing::Matcher<const Proto_LogMessage&> IsControl(std::optional<ra::type::FlightState> State)
{
    const auto Control = AllOf(
        Field("which_message_type", &Proto_MainMessage::which_message_type, Proto_MainMessage_status_tag),
        ResultOf("has_flight_state", [](const Proto_MainMessage& M) { return M.message_type.status.has_flight_state; }, State.has_value()));

    if (!State) { return Field("main_message", &Proto_LogMessage::main_message, Control); }

    const auto Wanted = static_cast<uint32_t>(*State);
    return Field("main_message",
                 &Proto_LogMessage::main_message,
                 AllOf(Control,
                       ResultOf("state",
                                [](const Proto_MainMessage& M) { return static_cast<uint32_t>(M.message_type.status.flight_state); },
                                Wanted)));
}

// Stands in for the storage a Logger flushes to: what it is asked to store, and how much of it
// it took. The Logger wants a plain function and a context pointer, so this is the context.
class MockStorage
{
public:
    MOCK_METHOD(uint32_t, Store, (std::span<const std::byte>));

    static uint32_t Callback(std::span<const std::byte> Data, void* Context)
    {
        return static_cast<MockStorage*>(Context)->Store(Data);
    }
};
} // namespace

class LoggerTest : public ::testing::Test
{
protected:
    LoggerTest() : m_Logger(ra::Logger::Instance()) {}

    static uint32_t CaptureStore(std::span<const std::byte> Data, void* Ctx)
    {
        auto* fixture = static_cast<LoggerTest*>(Ctx);
        if (fixture != nullptr) { fixture->m_Chunks.emplace_back(Data.begin(), Data.end()); }
        return static_cast<uint32_t>(Data.size_bytes());
    }

    // Forces any buffered data out by overfilling the internal buffer.
    // Keep this comfortably larger than production cache to force a flush path.
    static constexpr size_t kForceFlushWriteSize = 4096u;

    bool ForceFlush()
    {
        std::vector<std::byte> Trigger(kForceFlushWriteSize);
        return m_Logger.Log(std::span<const std::byte>(Trigger));
    }

    std::optional<Proto_LogMessage> FindDecodedMessageByTimestamp(uint32_t Timestamp)
    {
        for (const auto& Chunk : m_Chunks)
        {
            auto Remaining = std::span<const std::byte>(Chunk.data(), Chunk.size());
            while (!Remaining.empty())
            {
                const auto Frame = ra::turtleford::ProtoFrame_Read(Remaining);
                if (!Frame.has_value()) { break; }

                const auto Decoded = ra::turtleford::ProtoDecode_LogMessage(Frame->Payload);
                if (Decoded && Decoded->main_message.timestamp == Timestamp) { return Decoded; }

                Remaining = Remaining.subspan(Frame->BytesConsumed);
            }
        }
        return std::nullopt;
    }

    void SetUp() override
    {
        m_Logger.RegisterCallback(&CaptureStore, this);
        // Ensure buffer is clean for each test
        ForceFlush();
        m_Chunks.clear();
    }

    void TearDown() override
    {
        ForceFlush();
        m_Chunks.clear();
        // the Logger outlives this fixture, it must not keep pointing at it
        m_Logger.RegisterCallback(nullptr, nullptr);
    }

    ra::Logger& m_Logger;
    std::vector<std::vector<std::byte>> m_Chunks;
};

TEST_F(LoggerTest, InstanceReturnsSameReference)
{
    auto& InstanceA = ra::Logger::Instance();
    auto& InstanceB = ra::Logger::Instance();

    EXPECT_EQ(&InstanceA, &InstanceB);
}

TEST_F(LoggerTest, LogSerializedEmptySpanReturnsFalse)
{
    std::vector<std::byte> Empty {};

    EXPECT_FALSE(m_Logger.Log(std::span<const std::byte>(Empty)));
    EXPECT_THAT(m_Chunks, IsEmpty());
}

TEST_F(LoggerTest, LogSerializedSmallPayloadBuffersUntilFlush)
{
    std::array<std::byte, 1> Payload {std::byte {0x7A}};

    EXPECT_TRUE(m_Logger.Log(std::span<const std::byte>(Payload)));
    EXPECT_THAT(m_Chunks, IsEmpty());

    EXPECT_TRUE(ForceFlush());
    // Find our payload in the chunks (ForceFlush also adds a chunk)
    EXPECT_THAT(m_Chunks, testing::Contains(StartsWithBytes(std::vector<std::byte>(Payload.begin(), Payload.end()))));
}

TEST_F(LoggerTest, MultipleSmallLogsAreAggregated)
{
    std::array<std::byte, 10> Part1;
    Part1.fill(std::byte {0x11});
    std::array<std::byte, 10> Part2;
    Part2.fill(std::byte {0x22});

    m_Logger.Log(std::span<const std::byte>(Part1));
    m_Logger.Log(std::span<const std::byte>(Part2));

    EXPECT_THAT(m_Chunks, IsEmpty());

    ForceFlush();

    // The first chunk should contain both Part1 and Part2 because they were buffered together
    std::vector<std::byte> Both(Part1.begin(), Part1.end());
    Both.insert(Both.end(), Part2.begin(), Part2.end());
    ASSERT_THAT(m_Chunks, Not(IsEmpty()));
    EXPECT_THAT(m_Chunks.front(), StartsWithBytes(Both));
}

TEST_F(LoggerTest, FillingBufferToLimitTriggersFlushOnNextWrite)
{
    std::array<std::byte, 1> OneByte {std::byte {0xAA}};
    size_t Writes = 0;

    // Keep writing until the logger flushes due to no remaining cache space.
    while (m_Chunks.empty() && Writes < kForceFlushWriteSize)
    {
        EXPECT_TRUE(m_Logger.Log(std::span<const std::byte>(OneByte)));
        ++Writes;
    }

    ASSERT_THAT(m_Chunks, Not(IsEmpty()));
    ASSERT_GE(Writes, 2u);
    EXPECT_THAT(m_Chunks.front(), SizeIs(Writes - 1u));
    EXPECT_THAT(m_Chunks.front(), Each(std::byte {0xAA}));
}

TEST_F(LoggerTest, LogSerializedOversizedDataBypassesBuffer)
{
    std::array<std::byte, kForceFlushWriteSize> Payload {};
    Payload[0]                  = std::byte {0xCA};
    Payload[Payload.size() - 1] = std::byte {0xFE};

    EXPECT_TRUE(m_Logger.Log(std::span<const std::byte>(Payload)));

    // Should be in Chunks immediately
    ASSERT_THAT(m_Chunks, Not(IsEmpty()));
    EXPECT_THAT(m_Chunks.back(), ElementsAreArray(Payload));
}

TEST_F(LoggerTest, LogSerializedFlushesWhenIncomingDataExceedsRemainingSpace)
{
    std::array<std::byte, 1023> Head {};
    Head.fill(std::byte {0xA1});
    std::array<std::byte, 2> Tail {std::byte {0xB2}, std::byte {0xB3}};

    EXPECT_TRUE(m_Logger.Log(std::span<const std::byte>(Head)));
    EXPECT_TRUE(m_Logger.Log(std::span<const std::byte>(Tail)));

    ASSERT_THAT(m_Chunks, Not(IsEmpty()));
    EXPECT_THAT(m_Chunks.front(), ElementsAreArray(Head));

    EXPECT_TRUE(ForceFlush());
    ASSERT_GE(m_Chunks.size(), 2u);
    EXPECT_THAT(m_Chunks[1], ElementsAreArray(Tail));
}

TEST_F(LoggerTest, LogInfoEncodesTimestampSeverityAndMessage)
{
    const uint32_t Timestamp = RNG.Value<uint32_t>();
    const auto CategoryVal   = ra::type::Category::Application;
    const uint32_t Status    = RNG.Value<uint32_t>();
    const std::string Msg    = RNG.String(16);

    ra::Logger::LogInfo Info {
        .Timestamp = Timestamp,
        .Level     = ra::Logger::Severity::Warn,
        .Category  = CategoryVal,
    };

    EXPECT_TRUE(m_Logger.Log(Info, Status, Msg));
    EXPECT_TRUE(ForceFlush());

    ASSERT_GE(m_Chunks.size(), 1u);

    EXPECT_THAT(FindDecodedMessageByTimestamp(Timestamp),
                Optional(IsLog(static_cast<uint32_t>(ra::Logger::Severity::Warn), CategoryVal, Status)));
}

TEST_F(LoggerTest, CommandIsLoggedAsACommandMessage)
{
    const uint32_t Timestamp = RNG.Value<uint32_t>();

    EXPECT_TRUE(m_Logger.Log({.Timestamp = Timestamp,
                              .Level     = ra::Logger::Severity::Info,
                              .Category  = ra::type::Category::Communications},
                             ra::type::CommandMsg {ra::type::CommandType::Abort}));
    EXPECT_TRUE(ForceFlush());

    const auto Entry = FindDecodedMessageByTimestamp(Timestamp);
    ASSERT_TRUE(Entry.has_value());
    EXPECT_EQ(Entry->category, Proto_Category_CATEGORY_COMMUNICATIONS);
    ASSERT_EQ(Entry->main_message.which_message_type, Proto_MainMessage_command_tag);
    EXPECT_EQ(ra::turtleford::PbTo_CommandMsg(Entry->main_message.message_type.command).Command,
              ra::type::CommandType::Abort);
}

TEST_F(LoggerTest, AnyMainMessageIsLoggedAsItIsWithTheGivenTimestamp)
{
    const uint32_t Timestamp = RNG.Value<uint32_t>();

    EXPECT_TRUE(m_Logger.Log({.Timestamp = Timestamp,
                              .Level     = ra::Logger::Severity::Warn,
                              .Category  = ra::type::Category::Communications},
                             ra::turtleford::PbGen_SwitchChannelMsg(ra::type::TransportKind::Lora, 2)));
    EXPECT_TRUE(ForceFlush());

    const auto Entry = FindDecodedMessageByTimestamp(Timestamp);
    ASSERT_TRUE(Entry.has_value());
    EXPECT_EQ(Entry->severity, static_cast<uint32_t>(ra::Logger::Severity::Warn));
    ASSERT_EQ(Entry->main_message.which_message_type, Proto_MainMessage_switch_channel_tag);
    EXPECT_EQ(Entry->main_message.message_type.switch_channel.transport, Proto_Transport_TRANSPORT_LORA);
    EXPECT_EQ(Entry->main_message.message_type.switch_channel.channel, 2u);
}

TEST_F(LoggerTest, LoggingWithNowhereToStoreIsRefusedOnceTheCacheIsFullAndDoesNotCrash)
{
    m_Logger.RegisterCallback(nullptr, nullptr);

    const std::string Text(100, 'x');
    bool Refused = false;
    for (int i = 0; i < 100 && !Refused; ++i)
    {
        Refused = !m_Logger.Log({.Timestamp = 1,
                                 .Level     = ra::Logger::Severity::Info,
                                 .Category  = ra::type::Category::Application},
                                7,
                                Text);
    }
    EXPECT_TRUE(Refused) << "100 entries of 100 characters do not fit in the cache";

    // and it works again once there is storage
    m_Logger.RegisterCallback(&CaptureStore, this);
    EXPECT_TRUE(ForceFlush());
}

TEST_F(LoggerTest, LogInfoSupportsEmptyMessage)
{
    const uint32_t Timestamp = RNG.Value<uint32_t>();
    const auto CategoryVal   = ra::type::Category::Application;
    const uint32_t Status    = RNG.Value<uint32_t>();

    ra::Logger::LogInfo Info {
        .Timestamp = Timestamp,
        .Level     = ra::Logger::Severity::Info,
        .Category  = CategoryVal,
    };

    EXPECT_TRUE(m_Logger.Log(Info, Status, ""));
    EXPECT_TRUE(ForceFlush());

    EXPECT_THAT(FindDecodedMessageByTimestamp(Timestamp),
                Optional(IsLog(static_cast<uint32_t>(ra::Logger::Severity::Info), CategoryVal, Status)));
}

TEST_F(LoggerTest, LogFlightDataEncodesCorrectly)
{
    const uint32_t Timestamp = 555u;
    const auto CategoryVal   = ra::type::Category::Application;
    ra::Logger::LogInfo Info {
        .Timestamp = Timestamp,
        .Level     = ra::Logger::Severity::Info,
        .Category  = CategoryVal,
    };

    ra::type::FlightData Data {};
    Data.BMP_Data             = {1.1f, 2.2f, 3.3f};
    Data.AccelGyroTemperature = 4.4f;
    Data.Accel                = {.X = 5.5f, .Y = 6.6f, .Z = 7.7f};
    Data.Gyro                 = {.X = 8.8f, .Y = 9.9f, .Z = 10.1f};
    Data.Magnetometer         = {.X = 11.11f, .Y = 12.12f, .Z = 13.13f};
    Data.Thermometer          = 14.14f;

    EXPECT_TRUE(m_Logger.Log(Info, Data));
    EXPECT_TRUE(ForceFlush());

    const auto Decoded = FindDecodedMessageByTimestamp(Timestamp);
    ASSERT_TRUE(Decoded.has_value());

    EXPECT_EQ(Decoded->severity, static_cast<uint32_t>(ra::Logger::Severity::Info));
    EXPECT_EQ(Decoded->category, static_cast<uint32_t>(CategoryVal));
    const auto& Fd = Decoded->main_message.message_type.flight_data;
    EXPECT_FLOAT_EQ(Fd.bmp_data.temperature, Data.BMP_Data.Temperature);
    EXPECT_FLOAT_EQ(Fd.bmp_data.pressure, Data.BMP_Data.Pressure);
    EXPECT_FLOAT_EQ(Fd.bmp_data.altitude, Data.BMP_Data.Altitude);
    EXPECT_FLOAT_EQ(Fd.accel_gyro_temperature, Data.AccelGyroTemperature);
    EXPECT_FLOAT_EQ(Fd.accel.x, Data.Accel.X);
    EXPECT_FLOAT_EQ(Fd.accel.y, Data.Accel.Y);
    EXPECT_FLOAT_EQ(Fd.accel.z, Data.Accel.Z);
    EXPECT_FLOAT_EQ(Fd.gyro.x, Data.Gyro.X);
    EXPECT_FLOAT_EQ(Fd.gyro.y, Data.Gyro.Y);
    EXPECT_FLOAT_EQ(Fd.gyro.z, Data.Gyro.Z);
    EXPECT_FLOAT_EQ(Fd.magnetometer.x, Data.Magnetometer.X);
    EXPECT_FLOAT_EQ(Fd.magnetometer.y, Data.Magnetometer.Y);
    EXPECT_FLOAT_EQ(Fd.magnetometer.z, Data.Magnetometer.Z);
    EXPECT_FLOAT_EQ(Fd.thermometer, Data.Thermometer);
}

TEST_F(LoggerTest, LogFlightStateEncodesControlWithState)
{
    const uint32_t Timestamp = 901u;
    ra::Logger::LogInfo Info {
        .Timestamp = Timestamp,
        .Level     = ra::Logger::Severity::Info,
        .Category  = ra::type::Category::FlightControl,
    };

    const ra::type::StatusMsg StateData {.State = ra::type::FlightState::InFlight};

    EXPECT_TRUE(m_Logger.Log(Info, StateData));
    EXPECT_TRUE(ForceFlush());

    EXPECT_THAT(FindDecodedMessageByTimestamp(Timestamp), Optional(IsControl(ra::type::FlightState::InFlight)));
}

TEST_F(LoggerTest, LogFlightStateWithoutStateEncodesControlUnset)
{
    const uint32_t Timestamp = 902u;
    ra::Logger::LogInfo Info {
        .Timestamp = Timestamp,
        .Level     = ra::Logger::Severity::Warn,
        .Category  = ra::type::Category::FlightControl,
    };

    const ra::type::StatusMsg StateData {.State = std::nullopt};

    EXPECT_TRUE(m_Logger.Log(Info, StateData));
    EXPECT_TRUE(ForceFlush());

    EXPECT_THAT(FindDecodedMessageByTimestamp(Timestamp), Optional(IsControl(std::nullopt)));
}

// Every severity level must come back as it went in
class LoggerSeverityTest : public LoggerTest, public testing::WithParamInterface<ra::Logger::Severity>
{
};

TEST_P(LoggerSeverityTest, LogInfoSeverityRoundTrip)
{
    const auto Level       = GetParam();
    const auto Timestamp   = static_cast<uint32_t>(1000u + static_cast<uint32_t>(Level));
    const auto Msg         = std::string("sev-") + std::to_string(static_cast<uint32_t>(Level));
    const auto CategoryVal = ra::type::Category::Application;

    ra::Logger::LogInfo Info {
        .Timestamp = Timestamp,
        .Level     = Level,
        .Category  = CategoryVal,
    };

    const uint32_t Status = RNG.Value<uint32_t>();

    EXPECT_TRUE(m_Logger.Log(Info, Status, Msg));
    EXPECT_TRUE(ForceFlush());

    EXPECT_THAT(FindDecodedMessageByTimestamp(Timestamp), Optional(IsLog(static_cast<uint32_t>(Level), CategoryVal, Status)));
}

INSTANTIATE_TEST_SUITE_P(AllLevels,
                         LoggerSeverityTest,
                         testing::Values(ra::Logger::Severity::Verbose,
                                         ra::Logger::Severity::Info,
                                         ra::Logger::Severity::Warn,
                                         ra::Logger::Severity::Error),
                         [](const testing::TestParamInfo<ra::Logger::Severity>& Info)
                         {
                             switch (Info.param)
                             {
                                 case ra::Logger::Severity::Verbose: return std::string {"Verbose"};
                                 case ra::Logger::Severity::Info: return std::string {"Info"};
                                 case ra::Logger::Severity::Warn: return std::string {"Warn"};
                                 case ra::Logger::Severity::Error: return std::string {"Error"};
                             }
                             return std::string {"Unknown"};
                         });

TEST_F(LoggerTest, LogInfoLargeMessageIsSplitAndStillDecodes)
{
    const uint32_t Timestamp = 777u;
    // far longer than one message holds, with content that would expose reordering
    std::string Msg(kForceFlushWriteSize, ' ');
    for (size_t Index = 0; Index < Msg.size(); ++Index) { Msg[Index] = static_cast<char>('A' + Index % 23); }

    ra::Logger::LogInfo Info {
        .Timestamp = Timestamp, .Level = ra::Logger::Severity::Error, .Category = ra::type::Category::Communications};

    const uint32_t Status = RNG.Value<uint32_t>();

    EXPECT_TRUE(m_Logger.Log(Info, Status, Msg));
    EXPECT_TRUE(ForceFlush());

    // Every part is a complete entry of its own; joined in order they give the text back
    const size_t ExpectedParts =
        (Msg.size() + ra::turtleford::MaxDebugTextLength - 1) / ra::turtleford::MaxDebugTextLength;
    std::string Joined;
    size_t Parts = 0;
    for (const auto& Chunk : m_Chunks)
    {
        auto Remaining = std::span<const std::byte>(Chunk.data(), Chunk.size());
        while (!Remaining.empty())
        {
            const auto Frame = ra::turtleford::ProtoFrame_Read(Remaining);
            if (!Frame.has_value()) { break; }

            const auto Decoded = ra::turtleford::ProtoDecode_LogMessage(Frame->Payload);
            if (Decoded && Decoded->main_message.timestamp == Timestamp)
            {
                const auto& Debug = Decoded->main_message.message_type.debug;
                EXPECT_EQ(Decoded->main_message.which_message_type, Proto_MainMessage_debug_tag);
                EXPECT_EQ(Decoded->severity, static_cast<uint32_t>(Info.Level));
                EXPECT_EQ(Debug.status, Status);
                EXPECT_LE(std::string(Debug.msg).size(), ra::turtleford::MaxDebugTextLength);
                // counts down to 0 on the last part
                EXPECT_EQ(Decoded->main_message.chunks_left, ExpectedParts - 1 - Parts);

                Joined += Debug.msg;
                ++Parts;
            }

            Remaining = Remaining.subspan(Frame->BytesConsumed);
        }
    }

    EXPECT_EQ(Joined, Msg);
    EXPECT_EQ(Parts, ExpectedParts);
}

TEST_F(LoggerTest, LogInfoTextThatExactlyFillsOneMessageIsNotSplit)
{
    const uint32_t Timestamp = 778u;
    const std::string Msg(ra::turtleford::MaxDebugTextLength, 'F');

    ra::Logger::LogInfo Info {
        .Timestamp = Timestamp, .Level = ra::Logger::Severity::Info, .Category = ra::type::Category::Application};

    EXPECT_TRUE(m_Logger.Log(Info, 1u, Msg));
    EXPECT_TRUE(ForceFlush());

    EXPECT_THAT(FindDecodedMessageByTimestamp(Timestamp),
                Optional(Field("main_message",
                               &Proto_LogMessage::main_message,
                               AllOf(ResultOf("debug text",
                                              [](const Proto_MainMessage& M)
                                              { return std::string(M.message_type.debug.msg); },
                                              Msg),
                                     Field("chunks_left", &Proto_MainMessage::chunks_left, 0u)))));
}

TEST_F(LoggerTest, RegisterCallbackUsesProvidedContext)
{
    // the callback is called once, with everything that was written, through the context given
    testing::StrictMock<MockStorage> Storage;
    EXPECT_CALL(Storage, Store(SizeIs(kForceFlushWriteSize))).WillOnce([](std::span<const std::byte> Data) {
        return static_cast<uint32_t>(Data.size_bytes());
    });
    m_Logger.RegisterCallback(&MockStorage::Callback, &Storage);

    std::vector<std::byte> Oversized(kForceFlushWriteSize);
    EXPECT_TRUE(m_Logger.Log(std::span<const std::byte>(Oversized)));

    m_Logger.RegisterCallback(&CaptureStore, this);
    m_Chunks.clear();
    EXPECT_TRUE(ForceFlush());
}

TEST_F(LoggerTest, FlushHandsWhatIsCachedToStorage)
{
    const ra::type::StatusMsg State {.State = ra::type::FlightState::InFlight};
    ASSERT_TRUE(m_Logger.Log({.Timestamp = 4242, .Level = ra::Logger::Severity::Info, .Category = ra::type::Category::FlightControl},
                             State));
    EXPECT_FALSE(FindDecodedMessageByTimestamp(4242).has_value()) << "a small entry waits in the cache";

    m_Logger.Flush();

    EXPECT_TRUE(FindDecodedMessageByTimestamp(4242).has_value());
}
