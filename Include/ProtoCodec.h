#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

// encode and decode must be include before any proto headers
#include <pb_encode.h>
#include <pb_decode.h>
#include "ProtoMain.pb.h"

#include "Type.h"

namespace ra::turtleford
{
enum class ProtoFlags : uint8_t
{
    None   = 0,
    Framed = 1u << 0,
};

constexpr ProtoFlags operator|(ProtoFlags Left, ProtoFlags Right);
constexpr ProtoFlags operator&(ProtoFlags Left, ProtoFlags Right);
constexpr ProtoFlags& operator|=(ProtoFlags& Left, ProtoFlags Right);

struct ProtoFrame
{
    std::span<const std::byte> Payload;
    size_t BytesConsumed;
};
} // namespace ra::turtleford

namespace ra::turtleford
{
size_t ProtoWriteFrame(std::span<const std::byte> Payload, std::span<std::byte> Buffer);
std::optional<ProtoFrame> ProtoReadFrame(std::span<const std::byte> Data);

size_t ProtoEncode(const Proto_MainMessage& Message, std::span<std::byte> Buffer, ProtoFlags Flags = ProtoFlags::None);
size_t ProtoEncode(uint32_t TimeStamp,
                   uint32_t Severity,
                   type::Category Category,
                   const Proto_MainMessage& Message,
                   std::span<std::byte> Buffer,
                   ProtoFlags Flags = ProtoFlags::None);

std::optional<Proto_MainMessage> ProtoDecodeMain(std::span<const std::byte> Data, ProtoFlags Flags = ProtoFlags::None);
std::optional<Proto_LogMessage> ProtoDecodeLog(std::span<const std::byte> Data, ProtoFlags Flags = ProtoFlags::None);

// Util
Proto_MainMessage PbGen_FlightData(const type::FlightData& Data);
// WARNING : Underlying Str must live until after Encode
Proto_MainMessage PbGen_DebugMsg(uint32_t Status, const std::string& Str);
} // namespace ra::turtleford
