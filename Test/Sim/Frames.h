#pragma once

// Questions about a frame that a Radio put on a transport, for the Drop filters and taps of the end
// to end tests. A frame that does not decode is none of these. Each question also comes in a form
// that takes an already decoded message, for a filter that asks several.

#include <cstddef>
#include <optional>
#include <span>

#include "ProtoCodec.h"
#include "Type.h"

namespace sim
{
inline std::optional<Proto_MainMessage> DecodeFrame(std::span<const std::byte> Frame)
{
    return ra::turtleford::ProtoDecode_MainMessage(Frame);
}

// Carries a message of the kind Tag names, one of the Proto_MainMessage_*_tag values
inline bool CarriesTag(const Proto_MainMessage& Msg, pb_size_t Tag) { return Msg.which_message_type == Tag; }

inline bool IsSwitchRequest(const Proto_MainMessage& Msg)
{
    return CarriesTag(Msg, Proto_MainMessage_switch_channel_tag);
}

// Carries an ack, whatever else it carries
inline bool CarriesAck(const Proto_MainMessage& Msg) { return Msg.has_ack; }

// Numbered, so sent reliably, and of the kind Tag names if one is given
inline bool IsReliableFrame(const Proto_MainMessage& Msg, std::optional<pb_size_t> Tag = {})
{
    return Msg.has_seq && (!Tag.has_value() || CarriesTag(Msg, *Tag));
}

// Carries a command, that one if a command is given
inline bool IsCommandFrame(const Proto_MainMessage& Msg, std::optional<ra::type::CommandType> Command = {})
{
    if (!CarriesTag(Msg, Proto_MainMessage_command_tag)) { return false; }

    return !Command.has_value() || ra::turtleford::PbTo_CommandMsg(Msg.message_type.command).Command == *Command;
}

// The same questions about a frame on the wire
inline bool CarriesTag(std::span<const std::byte> Frame, pb_size_t Tag)
{
    const auto Decoded = DecodeFrame(Frame);
    return Decoded.has_value() && CarriesTag(*Decoded, Tag);
}

inline bool IsSwitchRequest(std::span<const std::byte> Frame)
{
    const auto Decoded = DecodeFrame(Frame);
    return Decoded.has_value() && IsSwitchRequest(*Decoded);
}

inline bool CarriesAck(std::span<const std::byte> Frame)
{
    const auto Decoded = DecodeFrame(Frame);
    return Decoded.has_value() && CarriesAck(*Decoded);
}

inline bool IsReliableFrame(std::span<const std::byte> Frame, std::optional<pb_size_t> Tag = {})
{
    const auto Decoded = DecodeFrame(Frame);
    return Decoded.has_value() && IsReliableFrame(*Decoded, Tag);
}

inline bool IsCommandFrame(std::span<const std::byte> Frame, std::optional<ra::type::CommandType> Command = {})
{
    const auto Decoded = DecodeFrame(Frame);
    return Decoded.has_value() && IsCommandFrame(*Decoded, Command);
}
} // namespace sim
