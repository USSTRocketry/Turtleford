#pragma once

#include <cstdint>
#include <string>
#include <span>
#include <vector>
#include <optional>

// encode and decode must be include before any proto headers
#include <pb_encode.h>
#include <pb_decode.h>
#include "ProtoMain.pb.h"

#include "Type.h"

// best used internally
namespace ra::turtleford
{
/**
 * @brief Encode a protobuf message into a buffer or compute required size.
 * @param Buffer : Destination buffer. If empty, the function returns the required size without writing.
 * @return : Number of bytes written or required.
 */
std::vector<std::byte> ProtoEncode(const Proto_MainMessage&);
size_t ProtoEncode(const Proto_MainMessage&, std::span<std::byte> Buffer);
// Log message
size_t ProtoEncode(
    uint32_t TimeStamp, uint32_t Severity, uint32_t Location, const Proto_MainMessage&, std::span<std::byte> Buffer);
std::vector<std::byte> ProtoEncode(uint32_t TimeStamp, uint32_t Severity, uint32_t Location, const Proto_MainMessage&);

std::optional<Proto_MainMessage> ProtoDecode_MainMessage(std::span<const std::byte> Data);
std::optional<Proto_LogMessage> ProtoDecode_LogMessage(std::span<const std::byte> Data);

// Util
Proto_MainMessage PbGen_FlightData(const type::FlightData& Data);
// WARNING : Underlying Str must live until after Encode
Proto_MainMessage PbGen_DebugMsg(uint32_t Status, const std::string& Str);

Proto_MainMessage PbGen_SwitchFrequencyMsg(float NewFrequency);

Proto_MainMessage PbGen_AckMsg(uint32_t ack_to);
} // namespace ra::turtleford
