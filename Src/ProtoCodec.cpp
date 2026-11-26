/**
 * @file ProtoCodec.cpp
 * @brief Implements the protobuf encoding, decoding and type conversion declared in ProtoCodec.h.
 */

#include "ProtoCodec.h"
#include "RocketGroundCommunication.pb.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace ra::turtleford
{
namespace
{
/** @brief The protobuf value of a flight state. The two enums share their value numbers. */
Proto_FlightState ToProtoFlightState(ra::type::FlightState State) { return static_cast<Proto_FlightState>(State); }

/** @brief A stream into the buffer, or one that only counts when the buffer is empty. */
pb_ostream_t OutputStream(std::span<std::byte> Buffer)
{
    return Buffer.empty() ? pb_ostream_t(PB_OSTREAM_SIZING)
                          : pb_ostream_from_buffer(reinterpret_cast<pb_byte_t*>(Buffer.data()), Buffer.size());
}

/**
 * @brief Encode a protobuf message into a buffer or compute required size.
 *
 * @tparam T      The nanopb message struct type.
 * @param Msg : Message struct to encode.
 * @param Buffer : Destination buffer. If empty, the function returns the required size without writing.
 * @param Flags : Nanopb encode flags, such as PB_ENCODE_DELIMITED. 0 for none.
 * @return : Number of bytes written or required. Returns 0 on encoding failure.
 */
template <typename T>
size_t PbEncode_Internal(const T& Msg, std::span<std::byte> Buffer, unsigned int Flags = 0)
{
    pb_ostream_t Stream = OutputStream(Buffer);

    const bool Encoded = Flags == 0 ? pb_encode(&Stream, nanopb::MessageDescriptor<T>::fields(), &Msg)
                                    : pb_encode_ex(&Stream, nanopb::MessageDescriptor<T>::fields(), &Msg, Flags);
    if (!Encoded) { return 0; }

    return Stream.bytes_written;
}

/** @brief Whether Value has Flag set. */
constexpr bool HasFlag(ProtoFlags Value, ProtoFlags Flag) { return (Value & Flag) != ProtoFlags::None; }

/** @brief The nanopb encode flags that match Flags. */
unsigned EncodeFlags(ProtoFlags Flags) { return HasFlag(Flags, ProtoFlags::Framed) ? PB_ENCODE_DELIMITED : 0u; }

/**
 * @brief Decode a protobuf message from a buffer.
 *
 * @tparam T     The nanopb message struct type.
 * @param Data   The encoded message.
 * @param Flags  ProtoFlags::Framed when Data starts with the message's length. Then Data must hold
 *               exactly that message.
 * @return The message, or nothing when it does not decode.
 */
template <typename T>
std::optional<T> PbDecode_Internal(std::span<const std::byte> Data, ProtoFlags Flags)
{
    T Msg {};

    auto Stream = pb_istream_from_buffer(reinterpret_cast<const pb_byte_t*>(Data.data()), Data.size());
    const bool Decoded =
        HasFlag(Flags, ProtoFlags::Framed)
            ? pb_decode_ex(&Stream, nanopb::MessageDescriptor<decltype(Msg)>::fields(), &Msg, PB_DECODE_DELIMITED)
            : pb_decode(&Stream, nanopb::MessageDescriptor<decltype(Msg)>::fields(), &Msg);
    if (!Decoded) { return std::nullopt; }
    if (HasFlag(Flags, ProtoFlags::Framed) && Stream.bytes_left != 0) { return std::nullopt; }

    return Msg;
}
} // namespace

size_t ProtoEncode(const Proto_MainMessage& Message, std::span<std::byte> Buffer, ProtoFlags Flags)
{ return PbEncode_Internal(Message, Buffer, EncodeFlags(Flags)); }

size_t ProtoEncode(uint32_t TimeStamp, Proto_MainMessage Message, std::span<std::byte> Buffer, ProtoFlags Flags)
{
    Message.timestamp = TimeStamp;
    return PbEncode_Internal(Message, Buffer, EncodeFlags(Flags));
}

size_t ProtoFrame_Write(std::span<const std::byte> Payload, std::span<std::byte> Buffer)
{
    pb_ostream_t Stream = OutputStream(Buffer);

    if (!pb_encode_varint(&Stream, Payload.size_bytes())) { return 0; }
    if (!pb_write(&Stream, reinterpret_cast<const pb_byte_t*>(Payload.data()), Payload.size_bytes())) { return 0; }

    return Stream.bytes_written;
}

size_t ProtoEncode(uint32_t TimeStamp,
                   uint32_t Severity,
                   type::Category Category,
                   Proto_MainMessage Message,
                   std::span<std::byte> Buffer,
                   ProtoFlags Flags)
{
    Message.timestamp = TimeStamp;

    const Proto_LogMessage Msg {
        .severity     = Severity,
        .category     = static_cast<Proto_Category>(Category),
        .main_message = Message,
    };

    return PbEncode_Internal(Msg, Buffer, EncodeFlags(Flags));
}

std::optional<Proto_MainMessage> ProtoDecode_MainMessage(std::span<const std::byte> Data, ProtoFlags Flags)
{ return PbDecode_Internal<Proto_MainMessage>(Data, Flags); }

std::optional<Proto_LogMessage> ProtoDecode_LogMessage(std::span<const std::byte> Data, ProtoFlags Flags)
{ return PbDecode_Internal<Proto_LogMessage>(Data, Flags); }

std::optional<ProtoFrame> ProtoFrame_Read(std::span<const std::byte> Data)
{
    auto Stream = pb_istream_from_buffer(reinterpret_cast<const pb_byte_t*>(Data.data()), Data.size());

    uint64_t PayloadSize = 0;
    if (!pb_decode_varint(&Stream, &PayloadSize) || PayloadSize > std::numeric_limits<size_t>::max())
    {
        return std::nullopt;
    }

    const auto HeaderSize   = Data.size() - Stream.bytes_left;
    const auto PayloadBytes = static_cast<size_t>(PayloadSize);
    // Compared without adding: a corrupt length near the maximum would wrap the sum
    if (PayloadBytes > Data.size() - HeaderSize) { return std::nullopt; }

    return ProtoFrame {.Payload = Data.subspan(HeaderSize, PayloadBytes), .BytesConsumed = HeaderSize + PayloadBytes};
}

// ---------------------------------------------------------------------------
// type -> proto
// ---------------------------------------------------------------------------

Proto_MainMessage PbGen_FlightData(const type::FlightData& Data)
{
    Proto_MainMessage Message  = Proto_MainMessage_init_zero;
    Message.which_message_type = Proto_MainMessage_flight_data_tag;

    auto& FD                  = Message.message_type.flight_data;
    FD.bmp_data.temperature   = Data.BMP_Data.Temperature;
    FD.bmp_data.pressure      = Data.BMP_Data.Pressure;
    FD.bmp_data.altitude      = Data.BMP_Data.Altitude;
    FD.accel_gyro_temperature = Data.AccelGyroTemperature;
    FD.accel                  = {.x = Data.Accel.X, .y = Data.Accel.Y, .z = Data.Accel.Z};
    FD.gyro                   = {.x = Data.Gyro.X, .y = Data.Gyro.Y, .z = Data.Gyro.Z};
    FD.magnetometer           = {.x = Data.Magnetometer.X, .y = Data.Magnetometer.Y, .z = Data.Magnetometer.Z};
    FD.thermometer            = Data.Thermometer;
    FD.gps_data.latitude      = Data.GPS_Data.Latitude;
    FD.gps_data.longitude     = Data.GPS_Data.Longitude;
    FD.gps_data.altitude      = Data.GPS_Data.Altitude;
    FD.gps_data.speed         = Data.GPS_Data.Speed;
    FD.gps_data.angle         = Data.GPS_Data.Angle;
    FD.gps_data.fix_quality   = Data.GPS_Data.FixQuality;
    FD.gps_data.satellites    = Data.GPS_Data.Satellites;

    return Message;
}

Proto_MainMessage PbGen_Status(const type::StatusMsg& Data)
{
    Proto_MainMessage Message  = Proto_MainMessage_init_zero;
    Message.which_message_type = Proto_MainMessage_status_tag;

    auto& Status            = Message.message_type.status;
    Status.has_flight_state = Data.State.has_value();
    Status.flight_state     = ToProtoFlightState(Data.State.value_or(type::FlightState::Unknown));

    return Message;
}

Proto_MainMessage PbGen_DebugMsg(uint32_t Status, std::string_view Str)
{
    Proto_MainMessage Message  = Proto_MainMessage_init_zero;
    Message.which_message_type = Proto_MainMessage_debug_tag;

    auto& Debug  = Message.message_type.debug;
    Debug.status = Status;

    // init_zero already terminated the string
    const auto Length = std::min(Str.size(), MaxDebugTextLength);
    std::memcpy(Debug.msg, Str.data(), Length);

    return Message;
}

Proto_MainMessage PbGen_DebugChunk(uint32_t Status, std::string_view Str, size_t Index)
{
    const size_t Count = DebugChunkCount(Str.size());
    if (Index >= Count) { Index = Count - 1; }

    Proto_MainMessage Message = PbGen_DebugMsg(Status, Str.substr(Index * MaxDebugTextLength, MaxDebugTextLength));
    Message.chunks_left       = static_cast<uint32_t>(Count - 1 - Index);

    return Message;
}

Proto_MainMessage PbGen_SwitchChannelMsg(type::TransportKind Transport, uint32_t Channel)
{
    Proto_MainMessage Message  = Proto_MainMessage_init_zero;
    Message.which_message_type = Proto_MainMessage_switch_channel_tag;

    auto& Switch     = Message.message_type.switch_channel;
    Switch.transport = static_cast<Proto_Transport>(Transport);
    Switch.channel   = Channel;

    return Message;
}

Proto_MainMessage PbGen_CommandMsg(type::CommandType Command)
{
    Proto_MainMessage Message  = Proto_MainMessage_init_zero;
    Message.which_message_type = Proto_MainMessage_command_tag;

    Message.message_type.command.type = static_cast<Proto_CommandType>(Command);

    return Message;
}

// ---------------------------------------------------------------------------
// proto -> type
// ---------------------------------------------------------------------------

type::FlightData PbTo_FlightData(const Proto_InFlightData& Data)
{
    type::FlightData Out {};

    Out.BMP_Data.Temperature = Data.bmp_data.temperature;
    Out.BMP_Data.Pressure    = Data.bmp_data.pressure;
    Out.BMP_Data.Altitude    = Data.bmp_data.altitude;
    Out.AccelGyroTemperature = Data.accel_gyro_temperature;
    Out.Accel                = {.X = Data.accel.x, .Y = Data.accel.y, .Z = Data.accel.z};
    Out.Gyro                 = {.X = Data.gyro.x, .Y = Data.gyro.y, .Z = Data.gyro.z};
    Out.Magnetometer         = {.X = Data.magnetometer.x, .Y = Data.magnetometer.y, .Z = Data.magnetometer.z};
    Out.Thermometer          = Data.thermometer;
    Out.GPS_Data.Latitude    = Data.gps_data.latitude;
    Out.GPS_Data.Longitude   = Data.gps_data.longitude;
    Out.GPS_Data.Altitude    = Data.gps_data.altitude;
    Out.GPS_Data.Speed       = Data.gps_data.speed;
    Out.GPS_Data.Angle       = Data.gps_data.angle;
    Out.GPS_Data.FixQuality  = Data.gps_data.fix_quality;
    Out.GPS_Data.Satellites  = Data.gps_data.satellites;

    return Out;
}

type::StatusMsg PbTo_Status(const Proto_Status& Data)
{
    type::StatusMsg Out {};
    if (Data.has_flight_state) { Out.State = static_cast<type::FlightState>(Data.flight_state); }
    return Out;
}

type::CommandMsg PbTo_CommandMsg(const Proto_Command& Data)
{ return type::CommandMsg {.Command = static_cast<type::CommandType>(Data.type)}; }

type::DebugMsg PbTo_DebugMsg(const Proto_DebugMessage& Data)
{
    const auto* End = static_cast<const char*>(std::memchr(Data.msg, '\0', sizeof(Data.msg)));
    const auto Size = End ? static_cast<size_t>(End - Data.msg) : sizeof(Data.msg);

    return type::DebugMsg {.Status = Data.status, .Text = std::string_view {Data.msg, Size}};
}
} // namespace ra::turtleford
