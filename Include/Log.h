#pragma once

#include <cstdint>
// currently no support for format
// #include <format>
#include <span>
#include <vector>
#include <string>

#include "Avionics_HAL.h"
#include "CachedBuffer.h"
#include "ProtoCodec.h"

namespace ra
{
class Logger
{
    constexpr static auto BufferSize = 512;
    using BufferType                 = bricks::CachedBuffer<BufferSize>;
    using SeverityType               = uint_fast16_t;

public:
    // Do not forget to update the severity map
    enum class Severity : SeverityType
    {
        Verbose = 0,
        Info,
        Warn,
        Error
    };

    struct LogInfo
    {
        uint32_t Timestamp;
        Severity Level;
        uint32_t Status;
        const std::string& Msg;
    };

    using StorageWriter = BufferType::StoreCallback;

public:
    static Logger& Instance()
    {
        static Logger Logger;
        return Logger;
    }

    bool Log(const LogInfo& Info)
    {
        const auto DebugMsg = turtleford::PbGen_DebugMsg(Info.Status, Info.Msg);
        const auto Data     = turtleford::ProtoEncode(Info.Timestamp, static_cast<uint32_t>(Info.Level), DebugMsg);

        return Log(std::span {Data.data(), Data.size()});
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
