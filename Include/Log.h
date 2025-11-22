#pragma once

#include <cstdint>
// currently no support for format
#include <format>
#include <span>
#include <vector>
#include <string_view>

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
        Verbose,
        Info,
        Warn,
        Error
    };

    using StorageWriter = BufferType::StoreCallback;

public:
    static Logger& Instance()
    {
        static Logger Logger;
        return Logger;
    }

    bool Log(uint32_t Timestamp, Severity Level, std::string_view Str)
    {
        const auto EncodedStr = turtleford::PbGen_DebugMsg(0, Str);
        const auto EncodeSize = turtleford::ProtoEncode(Timestamp, static_cast<SeverityType>(Level), &EncodedStr, {});

        std::vector<std::byte> Data {EncodeSize};
        turtleford::ProtoEncode(Timestamp, static_cast<SeverityType>(Level), EncodedStr, Data);

        return Log({Data.data(), Data.data() + EncodeSize});
    }

    // Serialized data ONLY
    bool Log(std::span<std::byte> Data)
    {
        if (Data.size_bytes() > 0) { return m_Buffer.Store(Data); }
        return false;
    }

    void RegisterCallback(StorageWriter Cbck) {}

private:
    Logger() {}

private:
    BufferType m_Buffer;
};
} // namespace ra
