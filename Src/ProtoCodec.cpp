#include "ProtoCodec.h"
#include "RocketGroundCommunication.pb.h"

#include <limits>
#include <memory>
#include <type_traits>

// Utils / callbacks
namespace
{
/**
 * @brief Encode a protobuf message into a buffer or compute required size.
 *
 * @param Msg : Message struct to encode.
 * @param Buffer : Destination buffer. If empty, the function returns the required size without writing.
 * @return : Number of bytes written or required. Returns 0 on encoding failure.
 */
template <typename T>
size_t PbEncode_Internal(const T& Msg, std::span<std::byte> Buffer, unsigned int Flags = 0)
{
    pb_ostream_t Stream = Buffer.empty()
                              ? pb_ostream_t(PB_OSTREAM_SIZING)
                              : pb_ostream_from_buffer(reinterpret_cast<pb_byte_t*>(Buffer.data()), Buffer.size());

    const bool Encoded = Flags == 0 ? pb_encode(&Stream, nanopb::MessageDescriptor<T>::fields(), &Msg)
                                    : pb_encode_ex(&Stream, nanopb::MessageDescriptor<T>::fields(), &Msg, Flags);
    if (!Encoded) { return 0; }

    return Stream.bytes_written;
}

bool PbDecode_Proto_MainMessage(pb_istream_t* Stream, const pb_field_t* Field, void** Arg)
{
    switch (Field->tag)
    {
        case Proto_MainMessage_debug_msg_tag:
        {
            auto* Msg = static_cast<Proto_DebugMessage*>(Field->pData);

            // this call back allocates a fitting string buffer to store incoming string
            Msg->msg.funcs.decode = [](pb_istream_t* Stream, const pb_field_iter_t* Field, void** Arg) -> bool
            {
                const auto Strlen = Stream->bytes_left;
                auto S            = std::make_unique<std::string>(Strlen, ' ');

                if (!pb_read(Stream, reinterpret_cast<pb_byte_t*>(S->data()), Strlen)) { return false; }

                // Transfer ownership to caller via raw pointer
                *Arg = S.release();
                return true;
            };
        }
        break;

        default:
            break;
    }
    return true;
}

} // namespace

namespace ra::turtleford
{
constexpr ProtoFlags operator|(ProtoFlags Left, ProtoFlags Right)
{
    return static_cast<ProtoFlags>(static_cast<uint8_t>(Left) | static_cast<uint8_t>(Right));
}

constexpr ProtoFlags operator&(ProtoFlags Left, ProtoFlags Right)
{
    return static_cast<ProtoFlags>(static_cast<uint8_t>(Left) & static_cast<uint8_t>(Right));
}

constexpr ProtoFlags& operator|=(ProtoFlags& Left, ProtoFlags Right)
{
    Left = Left | Right;
    return Left;
}

namespace
{
constexpr bool HasFlag(ProtoFlags Value, ProtoFlags Flag) { return (Value & Flag) != ProtoFlags::None; }

template <typename T>
std::optional<T> PbDecode_Internal(std::span<const std::byte> Data, ProtoFlags Flags)
{
    T Msg;

    if constexpr (std::is_same_v<T, Proto_MainMessage>)
    {
        Msg.cb_message_type.funcs.decode = PbDecode_Proto_MainMessage;
    }
    else if constexpr (std::is_same_v<T, Proto_LogMessage>)
    {
        Msg.main_message.cb_message_type.funcs.decode = PbDecode_Proto_MainMessage;
    }

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
{
    return PbEncode_Internal(Message, Buffer, HasFlag(Flags, ProtoFlags::Framed) ? PB_ENCODE_DELIMITED : 0u);
}

size_t ProtoWriteFrame(std::span<const std::byte> Payload, std::span<std::byte> Buffer)
{
    pb_ostream_t Stream = Buffer.empty()
                              ? pb_ostream_t(PB_OSTREAM_SIZING)
                              : pb_ostream_from_buffer(reinterpret_cast<pb_byte_t*>(Buffer.data()), Buffer.size());

    if (!pb_encode_varint(&Stream, Payload.size_bytes())) { return 0; }
    if (!pb_write(&Stream, reinterpret_cast<const pb_byte_t*>(Payload.data()), Payload.size_bytes())) { return 0; }

    return Stream.bytes_written;
}

size_t ProtoEncode(uint32_t TimeStamp,
                   uint32_t Severity,
                   type::Category Category,
                   const Proto_MainMessage& Message,
                   std::span<std::byte> Buffer,
                   ProtoFlags Flags)
{
    const Proto_LogMessage Msg {.time_stamp   = TimeStamp,
                                .severity     = Severity,
                                .category     = static_cast<Proto_Category>(Category),
                                .main_message = Message};
    return PbEncode_Internal(Msg, Buffer, HasFlag(Flags, ProtoFlags::Framed) ? PB_ENCODE_DELIMITED : 0u);
}

std::optional<Proto_MainMessage> ProtoDecodeMain(std::span<const std::byte> Data, ProtoFlags Flags)
{
    return PbDecode_Internal<Proto_MainMessage>(Data, Flags);
}

std::optional<Proto_LogMessage> ProtoDecodeLog(std::span<const std::byte> Data, ProtoFlags Flags)
{
    return PbDecode_Internal<Proto_LogMessage>(Data, Flags);
}

std::optional<ProtoFrame> ProtoReadFrame(std::span<const std::byte> Data)
{
    auto Stream = pb_istream_from_buffer(reinterpret_cast<const pb_byte_t*>(Data.data()), Data.size());

    uint64_t PayloadSize = 0;
    if (!pb_decode_varint(&Stream, &PayloadSize) || PayloadSize > std::numeric_limits<size_t>::max())
    {
        return std::nullopt;
    }

    const auto HeaderSize   = Data.size() - Stream.bytes_left;
    const auto PayloadBytes = static_cast<size_t>(PayloadSize);
    if (Data.size() < HeaderSize + PayloadBytes) { return std::nullopt; }

    return ProtoFrame {.Payload = Data.subspan(HeaderSize, PayloadBytes), .BytesConsumed = HeaderSize + PayloadBytes};
}

Proto_MainMessage PbGen_FlightData(const type::FlightData& Data)
{
    const Proto_InFlightData FD = {
        .has_control            = true,
        .control                = {.timestamp = Data.Timestamp},
        .bmp_data               = {.temperature = Data.BMP_Data.Temperature,
                                   .pressure    = Data.BMP_Data.Pressure,
                                   .altitude    = Data.BMP_Data.Altitude},
        .accel_gyro_temperature = Data.AccelGyroTemperature,
        .accel                  = {.X = Data.Accel.X, .Y = Data.Accel.Y, .Z = Data.Accel.Z},
        .gyro                   = {.X = Data.Gyro.X, .Y = Data.Gyro.Y, .Z = Data.Gyro.Z},
        .magnetometer           = {.X = Data.Magnetometer.X, .Y = Data.Magnetometer.Y, .Z = Data.Magnetometer.Z},
        .thermometer            = Data.Thermometer
    };

    return Proto_MainMessage {
        .which_message_type = Proto_MainMessage_in_flight_data_tag,
        .message_type       = {.in_flight_data = FD},
    };
}

Proto_MainMessage PbGen_DebugMsg(uint32_t Status, const std::string& Str)
{
    Proto_MainMessage Message {};
    Message.which_message_type = Proto_MainMessage_debug_msg_tag;

    auto& MsgStr  = Message.message_type.debug_msg;
    MsgStr.status = Status;

    MsgStr.msg.arg          = &const_cast<std::string&>(Str);
    MsgStr.msg.funcs.encode = [](pb_ostream_t* Stream, const pb_field_t* Fields, void* const* Arg) -> bool
    {
        const auto& S = *static_cast<const std::string*>(*Arg);
        return pb_encode_tag_for_field(Stream, Fields) &&
               pb_encode_string(Stream, reinterpret_cast<const uint8_t*>(S.data()), S.size());
    };

    return Message;
}
} // namespace ra::turtleford
