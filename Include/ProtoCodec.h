#pragma once

#include <cstdint>
#include <string>
#include <span>
#include <vector>

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
uint32_t ProtoEncode(const Proto_MainMessage&, std::span<std::byte> Buffer);
// Log message
uint32_t ProtoEncode(uint32_t TimeStamp, uint32_t Severity, const Proto_MainMessage&, std::span<std::byte> Buffer);
std::vector<std::byte> ProtoEncode(uint32_t TimeStamp, uint32_t Severity, const Proto_MainMessage&);

Proto_MainMessage ProtoDecode_MainMessage(std::span<const std::byte> Data);
Proto_LogMessage ProtoDecode_LogMessage(std::span<const std::byte> Data);

// Util
Proto_MainMessage PbGen_FlightData(const type::FlightData& Data);
// WARNING : Underlying Str must live until after Encode
Proto_MainMessage PbGen_DebugMsg(uint32_t Status, const std::string* Str);
} // namespace ra::turtleford
