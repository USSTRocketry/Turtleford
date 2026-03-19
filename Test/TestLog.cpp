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

static auto& RNG = ra::RNG::Instance();

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
                if (Decoded && Decoded->time_stamp == Timestamp) { return Decoded; }

                Remaining = Remaining.subspan(Frame->BytesConsumed);
            }
        }
        return std::nullopt;
    }

    bool ContainsChunkWithPrefix(std::span<const std::byte> Prefix)
    {
        for (const auto& Chunk : m_Chunks)
        {
            if (Chunk.size() < Prefix.size()) { continue; }

            bool Matches = true;
            for (size_t Index = 0; Index < Prefix.size(); ++Index)
            {
                if (Chunk[Index] != Prefix[Index])
                {
                    Matches = false;
                    break;
                }
            }

            if (Matches) { return true; }
        }
        return false;
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
    EXPECT_TRUE(m_Chunks.empty());
}

TEST_F(LoggerTest, LogSerializedSmallPayloadBuffersUntilFlush)
{
    std::array<std::byte, 1> Payload {std::byte {0x7A}};

    EXPECT_TRUE(m_Logger.Log(std::span<const std::byte>(Payload)));
    EXPECT_TRUE(m_Chunks.empty());

    EXPECT_TRUE(ForceFlush());
    ASSERT_GE(m_Chunks.size(), 1u);
    // Find our payload in the chunks (ForceFlush also adds a chunk)
    EXPECT_TRUE(ContainsChunkWithPrefix(std::span<const std::byte>(Payload)));
}

TEST_F(LoggerTest, MultipleSmallLogsAreAggregated)
{
    std::array<std::byte, 10> Part1;
    Part1.fill(std::byte {0x11});
    std::array<std::byte, 10> Part2;
    Part2.fill(std::byte {0x22});

    m_Logger.Log(std::span<const std::byte>(Part1));
    m_Logger.Log(std::span<const std::byte>(Part2));

    EXPECT_TRUE(m_Chunks.empty());

    ForceFlush();

    // The first chunk should contain both Part1 and Part2 because they were buffered together
    ASSERT_FALSE(m_Chunks.empty());
    const auto& FirstChunk = m_Chunks.front();
    ASSERT_GE(FirstChunk.size(), 20u);
    EXPECT_EQ(FirstChunk[0], std::byte {0x11});
    EXPECT_EQ(FirstChunk[10], std::byte {0x22});
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

    ASSERT_FALSE(m_Chunks.empty());
    ASSERT_GE(Writes, 2u);
    EXPECT_EQ(m_Chunks.front().size(), Writes - 1u);
    EXPECT_EQ(m_Chunks.front().front(), std::byte {0xAA});
}

TEST_F(LoggerTest, LogSerializedOversizedDataBypassesBuffer)
{
    std::array<std::byte, kForceFlushWriteSize> Payload {};
    Payload[0]                  = std::byte {0xCA};
    Payload[Payload.size() - 1] = std::byte {0xFE};

    EXPECT_TRUE(m_Logger.Log(std::span<const std::byte>(Payload)));

    // Should be in Chunks immediately
    ASSERT_FALSE(m_Chunks.empty());
    EXPECT_EQ(m_Chunks.back().size(), Payload.size());
    EXPECT_EQ(m_Chunks.back().front(), std::byte {0xCA});
    EXPECT_EQ(m_Chunks.back().back(), std::byte {0xFE});
}

TEST_F(LoggerTest, LogSerializedFlushesWhenIncomingDataExceedsRemainingSpace)
{
    std::array<std::byte, 1023> Head {};
    Head.fill(std::byte {0xA1});
    std::array<std::byte, 2> Tail {std::byte {0xB2}, std::byte {0xB3}};

    EXPECT_TRUE(m_Logger.Log(std::span<const std::byte>(Head)));
    EXPECT_TRUE(m_Logger.Log(std::span<const std::byte>(Tail)));

    ASSERT_FALSE(m_Chunks.empty());
    EXPECT_EQ(m_Chunks.front().size(), Head.size());
    EXPECT_EQ(m_Chunks.front().front(), std::byte {0xA1});

    EXPECT_TRUE(ForceFlush());
    ASSERT_GE(m_Chunks.size(), 2u);
    EXPECT_EQ(m_Chunks[1].size(), 2u);
    EXPECT_EQ(m_Chunks[1][0], std::byte {0xB2});
    EXPECT_EQ(m_Chunks[1][1], std::byte {0xB3});
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

    const auto Decoded = FindDecodedMessageByTimestamp(Timestamp);
    ASSERT_TRUE(Decoded.has_value());

    EXPECT_EQ(Decoded->severity, static_cast<uint32_t>(ra::Logger::Severity::Warn));
    EXPECT_EQ(Decoded->category, static_cast<uint32_t>(CategoryVal));
    EXPECT_EQ(Decoded->main_message.message_type.debug_msg.status, Status);
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

    const auto Decoded = FindDecodedMessageByTimestamp(Timestamp);
    ASSERT_TRUE(Decoded.has_value());

    EXPECT_EQ(Decoded->severity, static_cast<uint32_t>(ra::Logger::Severity::Info));
    EXPECT_EQ(Decoded->category, static_cast<uint32_t>(CategoryVal));
    EXPECT_EQ(Decoded->main_message.message_type.debug_msg.status, Status);
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
    Data.Timestamp            = 42u;
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
    const auto& Fd = Decoded->main_message.message_type.in_flight_data;
    EXPECT_TRUE(Fd.has_control);
    EXPECT_EQ(Fd.control.timestamp, Data.Timestamp);
    EXPECT_FLOAT_EQ(Fd.bmp_data.temperature, Data.BMP_Data.Temperature);
    EXPECT_FLOAT_EQ(Fd.bmp_data.pressure, Data.BMP_Data.Pressure);
    EXPECT_FLOAT_EQ(Fd.bmp_data.altitude, Data.BMP_Data.Altitude);
    EXPECT_FLOAT_EQ(Fd.accel_gyro_temperature, Data.AccelGyroTemperature);
    EXPECT_FLOAT_EQ(Fd.accel.X, Data.Accel.X);
    EXPECT_FLOAT_EQ(Fd.accel.Y, Data.Accel.Y);
    EXPECT_FLOAT_EQ(Fd.accel.Z, Data.Accel.Z);
    EXPECT_FLOAT_EQ(Fd.gyro.X, Data.Gyro.X);
    EXPECT_FLOAT_EQ(Fd.gyro.Y, Data.Gyro.Y);
    EXPECT_FLOAT_EQ(Fd.gyro.Z, Data.Gyro.Z);
    EXPECT_FLOAT_EQ(Fd.magnetometer.X, Data.Magnetometer.X);
    EXPECT_FLOAT_EQ(Fd.magnetometer.Y, Data.Magnetometer.Y);
    EXPECT_FLOAT_EQ(Fd.magnetometer.Z, Data.Magnetometer.Z);
    EXPECT_FLOAT_EQ(Fd.thermometer, Data.Thermometer);
}

TEST_F(LoggerTest, LogInfoSeverityRoundTripAllLevels)
{
    const std::array<ra::Logger::Severity, 4> Levels {
        ra::Logger::Severity::Verbose,
        ra::Logger::Severity::Info,
        ra::Logger::Severity::Warn,
        ra::Logger::Severity::Error,
    };

    for (size_t i = 0; i < Levels.size(); ++i)
    {
        m_Chunks.clear();
        const auto Msg         = std::string("sev-") + std::to_string(i);
        const auto CategoryVal = ra::type::Category::Application;

        ra::Logger::LogInfo Info {
            .Timestamp = static_cast<uint32_t>(1000u + i),
            .Level     = Levels[i],
            .Category  = CategoryVal,
        };

        const uint32_t Status = RNG.Value<uint32_t>();

        EXPECT_TRUE(m_Logger.Log(Info, Status, Msg));
        EXPECT_TRUE(ForceFlush());

        const auto Decoded = FindDecodedMessageByTimestamp(static_cast<uint32_t>(1000u + i));
        ASSERT_TRUE(Decoded.has_value());

        EXPECT_EQ(Decoded->severity, static_cast<uint32_t>(Levels[i]));
        EXPECT_EQ(Decoded->category, static_cast<uint32_t>(CategoryVal));
        EXPECT_EQ(Decoded->main_message.message_type.debug_msg.status, Status);
    }
}

TEST_F(LoggerTest, LogInfoLargeMessageStillDecodes)
{
    const uint32_t Timestamp = 777u;
    const std::string Msg(kForceFlushWriteSize, 'L');

    ra::Logger::LogInfo Info {
        .Timestamp = Timestamp, .Level = ra::Logger::Severity::Error, .Category = ra::type::Category::Communications};

    const uint32_t Status = RNG.Value<uint32_t>();

    // The message is too large for the fixed encoding buffer; should be rejected.
    EXPECT_FALSE(m_Logger.Log(Info, Status, Msg));
    EXPECT_TRUE(m_Chunks.empty());
}

TEST_F(LoggerTest, RegisterCallbackUsesProvidedContext)
{
    struct LocalState
    {
        uint32_t CallCount = 0;
        uint32_t LastBytes = 0;
    };

    static auto LocalCallback = [](std::span<const std::byte> Data, void* Ctx) -> uint32_t
    {
        auto* State = static_cast<LocalState*>(Ctx);
        if (State != nullptr)
        {
            State->CallCount += 1;
            State->LastBytes = static_cast<uint32_t>(Data.size_bytes());
        }
        return static_cast<uint32_t>(Data.size_bytes());
    };

    LocalState State {};
    m_Logger.RegisterCallback(LocalCallback, &State);

    std::vector<std::byte> Oversized(kForceFlushWriteSize);
    EXPECT_TRUE(m_Logger.Log(std::span<const std::byte>(Oversized)));

    EXPECT_EQ(State.CallCount, 1u);
    EXPECT_EQ(State.LastBytes, static_cast<uint32_t>(Oversized.size()));

    m_Logger.RegisterCallback(&CaptureStore, this);
    m_Chunks.clear();
    EXPECT_TRUE(ForceFlush());
}
