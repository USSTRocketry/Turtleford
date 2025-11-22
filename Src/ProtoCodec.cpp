#include "ProtoCodec.h"
#include "FlightComputerGroundStationCommunication/RocketGroundCommunication.pb.h"
#include <string>
#include <memory>

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
uint32_t PbEncode_Internal(const T& Msg, std::span<std::byte> Buffer)
{
    pb_ostream_t Stream = Buffer.empty()
                              ? pb_ostream_t(PB_OSTREAM_SIZING)
                              : pb_ostream_from_buffer(reinterpret_cast<pb_byte_t*>(Buffer.data()), Buffer.size());

    if (!pb_encode(&Stream, nanopb::MessageDescriptor<T>::fields(), &Msg)) { return 0; }

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
uint32_t ProtoEncode(const Proto_MainMessage& Message, std::span<std::byte> Buffer)
{
    return PbEncode_Internal(Message, Buffer);
}
uint32_t
    ProtoEncode(uint32_t TimeStamp, uint32_t Severity, const Proto_MainMessage& Message, std::span<std::byte> Buffer)
{
    const Proto_LogMessage Msg {.time_stamp = TimeStamp, .severity = Severity, .main_message = Message};
    return PbEncode_Internal(Msg, Buffer);
}

Proto_MainMessage ProtoDecode_MainMessage(std::span<const std::byte> Data)
{
    Proto_MainMessage Msg;
    Msg.cb_message_type.funcs.decode = PbDecode_Proto_MainMessage;

    auto Stream = pb_istream_from_buffer(reinterpret_cast<const pb_byte_t*>(Data.data()), Data.size());
    if (!pb_decode(&Stream, nanopb::MessageDescriptor<decltype(Msg)>::fields(), &Msg)) { return {}; }

    return Msg;
}

Proto_LogMessage ProtoDecode_LogMessage(std::span<const std::byte> Data)
{
    Proto_LogMessage Msg;
    Msg.main_message.cb_message_type.funcs.decode = PbDecode_Proto_MainMessage;

    auto Stream = pb_istream_from_buffer(reinterpret_cast<const pb_byte_t*>(Data.data()), Data.size());
    if (!pb_decode(&Stream, nanopb::MessageDescriptor<decltype(Msg)>::fields(), &Msg)) { return {}; }

    return Msg;
}

// Util
std::vector<std::byte> ProtoEncode(const Proto_MainMessage& MainMsg)
{
    const auto Size = ProtoEncode(MainMsg, {});
    std::vector<std::byte> Buffer {Size};
    ProtoEncode(MainMsg, Buffer);

    return Buffer;
}
std::vector<std::byte> ProtoEncode(uint32_t TimeStamp, uint32_t Severity, const Proto_MainMessage& MainMsg)
{
    const auto Size = ProtoEncode(TimeStamp, Severity, MainMsg, {});
    std::vector<std::byte> Buffer {Size};
    ProtoEncode(TimeStamp, Severity, MainMsg, Buffer);

    return Buffer;
}

Proto_MainMessage PbGen_FlightData(const type::FlightData& Data)
{
    const Proto_InFlightData FD = {
        .timestamp_ms           = Data.TimestampMs,
        .bmp_data               = {.temperature = Data.BMP_Data.Temperature,
                                   .pressure    = Data.BMP_Data.Pressure,
                                   .altitude    = Data.BMP_Data.Altitude                                                       },
        .accel_gyro_temperature = Data.AccelGyroTemperature,
        .accel                  = {                       .X = Data.Accel.X,        .Y = Data.Accel.Y,        .Z = Data.Accel.Z},
        .gyro                   = {                        .X = Data.Gyro.X,         .Y = Data.Gyro.Y,         .Z = Data.Gyro.Z},
        .magnetometer           = {                .X = Data.Magnetometer.X, .Y = Data.Magnetometer.Y, .Z = Data.Magnetometer.Z},
        .thermometer            = Data.Thermometer
    };

    return Proto_MainMessage {
        .which_message_type = Proto_MainMessage_in_flight_data_tag,
        .message_type       = {.in_flight_data = FD},
    };
}

Proto_MainMessage PbGen_DebugMsg(uint32_t Status, const std::string* Str)
{
    Proto_MainMessage Message {};
    Message.which_message_type = Proto_MainMessage_debug_msg_tag;

    auto& MsgStr  = Message.message_type.debug_msg;
    MsgStr.status = Status;

    MsgStr.msg.arg          = const_cast<std::string*>(Str);
    MsgStr.msg.funcs.encode = [](pb_ostream_t* Stream, const pb_field_t* Fields, void* const* Arg) -> bool
    {
        const auto& S = *static_cast<const std::string*>(*Arg);
        return pb_encode_tag_for_field(Stream, Fields) &&
               pb_encode_string(Stream, reinterpret_cast<const uint8_t*>(S.c_str()), S.size());
    };

    return Message;
}
} // namespace ra::turtleford
