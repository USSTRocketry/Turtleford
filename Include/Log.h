#pragma once

#include <cstdint>
// currently no support for format
// #include <format>
#include <span>
#include <vector>
#include <string>

#include "Avionics_HAL.h"
#include "DataStructure/Buffer/CachedBuffer.h"
#include "ProtoCodec.h"
#include "Type.h"

namespace ra
{
class Logger
{
    constexpr static auto BufferSize = 512;
    using BufferType                 = bricks::CachedBuffer<BufferSize>;
    using EnumType                   = uint_fast16_t;

public:
    // Do not forget to update the severity map
    enum class Severity : EnumType
    {
        Verbose = 0,
        Info,
        Warn,
        Error
    };

    enum class Module : EnumType
    {
        Turtleford
    };

    struct LogInfo
    {
        uint32_t Timestamp;
        Severity Level;
        Module Location;
    };

    using StorageWriter = BufferType::StoreCallback;

public:
    static Logger& Instance()
    {
        static Logger Logger;
        return Logger;
    }

    // TODO : Separate package logic (to proto) from storing logic (writing to storage)
    bool Log(const LogInfo& Info, const ra::turtleford::type::FlightData& Data)
    {
        // package incoming flight data for protobuf encoding
        const auto FlightData = turtleford::PbGen_FlightData(Data);
        const auto Encoded    = turtleford::ProtoEncode(
            Info.Timestamp, static_cast<uint32_t>(Info.Level), static_cast<uint32_t>(Info.Location), FlightData);

        return Log(std::span {Encoded.data(), Encoded.size()});
    }

    bool Log(const LogInfo& Info, const std::string& Msg)
    {
        const auto DebugMsg = turtleford::PbGen_DebugMsg(static_cast<uint32_t>(Info.Location), Msg);
        const auto Encoded  = turtleford::ProtoEncode(
            Info.Timestamp, static_cast<uint32_t>(Info.Level), static_cast<uint32_t>(Info.Location), DebugMsg);

        return Log(std::span {Encoded.data(), Encoded.size()});
    }

    // Serialized data ONLY
    bool Log(std::span<const std::byte> Data)
    {
        if (Data.size_bytes() > 0) { return m_Buffer.Store(Data); }
        return false;
    }

    void RegisterCallback(StorageWriter Cb, void* Ctx) { m_Buffer.RegisterCallback(Cb, Ctx); }

private:
    Logger() {}
    Logger(Logger&)             = delete;
    Logger(Logger&&)            = delete;
    Logger operator=(Logger&)   = delete;
    Logger& operator=(Logger&&) = delete;

private:
    BufferType m_Buffer;
};
} // namespace ra
