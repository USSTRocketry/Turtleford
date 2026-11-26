/**
 * @file ProtoCodec.h
 * @brief Encoding and decoding of the protobuf messages (nanopb), and conversion between the
 * protobuf structs and the plain types in Type.h.
 *
 * Implemented in ProtoCodec.cpp. The encode functions write into a caller supplied buffer and
 * return the number of bytes written, or 0 on failure. With an empty buffer they write nothing and
 * return the size the message needs.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

// encode and decode must be include before any proto headers
#include <pb_encode.h>
#include <pb_decode.h>
#include "ProtoMain.pb.h"

#include "Type.h"

namespace ra::turtleford
{
/** @brief Options of the encode and decode functions. Combine with operator|. */
enum class ProtoFlags : uint8_t
{
    None   = 0,       ///< The bare message.
    Framed = 1u << 0, ///< The message is preceded by its length (a protobuf varint), so a stream can be split.
};

// constexpr, so defined here: every file that uses them needs the definition
/** @brief The flags set in either operand. */
constexpr ProtoFlags operator|(ProtoFlags Left, ProtoFlags Right)
{ return static_cast<ProtoFlags>(static_cast<uint8_t>(Left) | static_cast<uint8_t>(Right)); }

/** @brief The flags set in both operands. */
constexpr ProtoFlags operator&(ProtoFlags Left, ProtoFlags Right)
{ return static_cast<ProtoFlags>(static_cast<uint8_t>(Left) & static_cast<uint8_t>(Right)); }

/** @brief Sets the flags of Right in Left. Returns Left. */
constexpr ProtoFlags& operator|=(ProtoFlags& Left, ProtoFlags Right)
{
    Left = Left | Right;
    return Left;
}

/** @brief One length prefixed message found in a stream, as ProtoFrame_Read() returns it. */
struct ProtoFrame
{
    std::span<const std::byte> Payload; ///< The message bytes, without the length prefix. Points into the data read.
    size_t BytesConsumed;               ///< Bytes of the data the frame took: the length prefix plus the payload.
};
} // namespace ra::turtleford

namespace ra::turtleford
{
/**
 * @brief Writes Payload preceded by its length (a protobuf varint).
 * @param Payload  The bytes to frame.
 * @param Buffer   Where to write. Empty to only find out how many bytes it takes.
 * @return Bytes written (or needed, for an empty Buffer), 0 when it does not fit.
 */
size_t ProtoFrame_Write(std::span<const std::byte> Payload, std::span<std::byte> Buffer);
/**
 * @brief Reads one length prefixed frame from the start of Data.
 * @param Data  Bytes that start with a frame. Anything after it is left alone.
 * @return The frame, or nothing when the length prefix is bad or the data holds less than it says.
 */
std::optional<ProtoFrame> ProtoFrame_Read(std::span<const std::byte> Data);

/**
 * @brief Encodes Message exactly as given (timestamp and link fields untouched).
 * @param Message  The message to encode.
 * @param Buffer   Where to write. Empty to only find out how many bytes it takes.
 * @param Flags    ProtoFlags::Framed to put the message's length in front.
 * @return Bytes written (or needed, for an empty Buffer), 0 when it does not fit or cannot be encoded.
 */
size_t ProtoEncode(const Proto_MainMessage& Message,
                   std::span<std::byte> Buffer,
                   ProtoFlags Flags = ProtoFlags::None);
/**
 * @brief Encodes Message with its timestamp set to TimeStamp.
 * @param TimeStamp  Written to the message's timestamp field.
 * @param Message    The message to encode. Taken by value: the caller's copy is not changed.
 * @param Buffer     Where to write. Empty to only find out how many bytes it takes.
 * @param Flags      ProtoFlags::Framed to put the message's length in front.
 * @return Bytes written (or needed, for an empty Buffer), 0 when it does not fit or cannot be encoded.
 */
size_t ProtoEncode(uint32_t TimeStamp,
                   Proto_MainMessage Message,
                   std::span<std::byte> Buffer,
                   ProtoFlags Flags = ProtoFlags::None);
/**
 * @brief Encodes Message as a log entry (Proto_LogMessage): the message plus its severity and category,
 * with its timestamp set to TimeStamp. Decode it with ProtoDecode_LogMessage().
 * @param TimeStamp  Written to the message's timestamp field.
 * @param Severity   The log level, as a Logger::Severity value.
 * @param Category   Where the entry comes from.
 * @param Message    The message to log. Taken by value: the caller's copy is not changed.
 * @param Buffer     Where to write. Empty to only find out how many bytes it takes.
 * @param Flags      ProtoFlags::Framed to put the entry's length in front.
 * @return Bytes written (or needed, for an empty Buffer), 0 when it does not fit or cannot be encoded.
 */
size_t ProtoEncode(uint32_t TimeStamp,
                   uint32_t Severity,
                   type::Category Category,
                   Proto_MainMessage Message,
                   std::span<std::byte> Buffer,
                   ProtoFlags Flags = ProtoFlags::None);

/**
 * @brief Decodes a Proto_MainMessage.
 * @param Data   The encoded message.
 * @param Flags  ProtoFlags::Framed when Data starts with the message's length. Then Data must hold
 *               exactly that message and nothing more.
 * @return The message, or nothing when Data does not decode.
 */
std::optional<Proto_MainMessage> ProtoDecode_MainMessage(std::span<const std::byte> Data,
                                                         ProtoFlags Flags = ProtoFlags::None);
/**
 * @brief Decodes a Proto_LogMessage, as written by the log entry ProtoEncode().
 * @param Data   The encoded entry.
 * @param Flags  ProtoFlags::Framed when Data starts with the entry's length. Then Data must hold
 *               exactly that entry and nothing more.
 * @return The entry, or nothing when Data does not decode.
 */
std::optional<Proto_LogMessage> ProtoDecode_LogMessage(std::span<const std::byte> Data,
                                                       ProtoFlags Flags = ProtoFlags::None);

// type -> proto
/**
 * @brief A flight data message.
 * @param Data  The sensor readings.
 */
Proto_MainMessage PbGen_FlightData(const type::FlightData& Data);
/**
 * @brief A status message.
 * @param Data  The status. A flight state that is not set is left out of the message.
 */
Proto_MainMessage PbGen_Status(const type::StatusMsg& Data);
/**
 * @brief A debug text message. Str is copied into the message and truncated to MaxDebugTextLength.
 * @param Status  The status code that goes with the text.
 * @param Str     The text.
 */
Proto_MainMessage PbGen_DebugMsg(uint32_t Status, std::string_view Str);
/**
 * @brief Part Index (from 0) of Str split into DebugChunkCount() messages, chunks_left filled in.
 * @param Status  The status code that goes with the text.
 * @param Str     The whole text.
 * @param Index   Which part to build. An Index past the last part gives the last part.
 */
Proto_MainMessage PbGen_DebugChunk(uint32_t Status, std::string_view Str, size_t Index);
/**
 * @brief A request to the peer to move one of its transports to another channel.
 * @param Transport  The transport to move.
 * @param Channel    The channel, a number whose meaning belongs to that transport.
 */
Proto_MainMessage PbGen_SwitchChannelMsg(type::TransportKind Transport, uint32_t Channel);
/**
 * @brief A command message.
 * @param Command  The command for the peer to carry out.
 */
Proto_MainMessage PbGen_CommandMsg(type::CommandType Command);

// proto -> type
/**
 * @brief The sensor readings of a flight data message.
 * @param Data  The protobuf flight data.
 */
type::FlightData PbTo_FlightData(const Proto_InFlightData& Data);
/**
 * @brief The status of a status message. The flight state is unset when the message does not carry one.
 * @param Data  The protobuf status.
 */
type::StatusMsg PbTo_Status(const Proto_Status& Data);
/**
 * @brief The command of a command message.
 * @param Data  The protobuf command.
 */
type::CommandMsg PbTo_CommandMsg(const Proto_Command& Data);
/**
 * @brief The status and text of a debug message. The returned Text points into Data.
 * @param Data  The protobuf debug message. Must outlive the returned text.
 */
type::DebugMsg PbTo_DebugMsg(const Proto_DebugMessage& Data);

/** @brief Longest debug string that fits in a message, in bytes. */
constexpr size_t MaxDebugTextLength = sizeof(Proto_DebugMessage::msg) - 1;

/**
 * @brief Number of messages a debug string of this length is carried in; empty text still takes one.
 * @param Length  The length of the text, in bytes.
 */
constexpr size_t DebugChunkCount(size_t Length)
{ return Length == 0 ? 1 : (Length + MaxDebugTextLength - 1) / MaxDebugTextLength; }
} // namespace ra::turtleford
