/**
 * @file Type.h
 * @brief The plain data types the library's API uses, apart from their protobuf form.
 *
 * ProtoCodec.h converts between these and the generated protobuf structs. Enums that mirror a
 * protobuf enum keep its value numbers.
 */

#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

/** @brief The plain data types the library's API uses, apart from their protobuf form. */
namespace ra::type
{
/**
 * @brief Three values of one type, one per axis.
 * @tparam T The type of each axis value.
 */
template <typename T>
struct Vector3
{
    T X; ///< Value along the X axis.
    T Y; ///< Value along the Y axis.
    T Z; ///< Value along the Z axis.
};

/** @brief The state of the flight computer. Mirrors the protobuf FlightState. */
enum class FlightState : uint32_t
{
    // NOTE: mirrors Proto.FlightState, keep value numbers in sync
    Unknown    = 0, ///< Not known, or not reported.
    Unarmed    = 1, ///< Not armed.
    GroundIdle = 2, ///< Idle on the ground.
    InFlight   = 3, ///< In flight.
    MainChute  = 4, ///< Main parachute stage.
};

/** @brief One reading of every sensor on the rocket. */
struct FlightData
{
    /** @brief Three floats, as used for the accelerometer, gyroscope and magnetometer. */
    using Vector3f = Vector3<float>;
    /** @brief Barometer readings. */
    struct BMP_Info
    {
        float Temperature; ///< Temperature from the barometer.
        float Pressure;    ///< Air pressure.
        float Altitude;    ///< Altitude from the barometer.
    };
    /** @brief GPS readings. */
    struct GPS_Info
    {
        float Latitude;      ///< Latitude.
        float Longitude;     ///< Longitude.
        float Altitude;      ///< Altitude from the GPS.
        float Speed;         ///< Speed.
        float Angle;         ///< Angle (heading) from the GPS.
        uint32_t FixQuality; ///< The fix quality the GPS reports.
        uint32_t Satellites; ///< Satellites in use.
    };

    BMP_Info BMP_Data;          ///< The barometer.
    float AccelGyroTemperature; ///< Temperature of the accelerometer and gyroscope chip.
    Vector3f Accel;             ///< The accelerometer.
    Vector3f Gyro;              ///< The gyroscope.
    Vector3f Magnetometer;      ///< The magnetometer.
    float Thermometer;          ///< Reading of the separate thermometer.
    GPS_Info GPS_Data;          ///< The GPS.
};

/** @brief What the sender is. Each field is optional: a message carries only what it reports. */
struct StatusMsg
{
    std::optional<FlightState> State; ///< The flight state, when the message reports it.
};

/**
 * @brief The part of the system a log entry comes from.
 *
 * The values are serialized into log history: never change or reorder existing entries.
 */
enum class Category : uint32_t
{
    // NOTE: These values are serialized into log history.
    // Never change or reorder existing entries.
    Unknown        = 0, ///< Not specified.
    Communications = 1, ///< Communications, such as the radio.
    Application    = 2, ///< The application.
    Platform       = 3, ///< The platform layer.
    FlightControl  = 4, ///< Flight control.
    Sensors        = 5, ///< The sensors.
    Storage        = 6, ///< Storage.
    Tasking        = 7, ///< Tasking.
};

/** @brief A command to the rocket. Mirrors the protobuf CommandType. */
enum class CommandType : uint32_t
{
    // NOTE: mirrors Proto.CommandType, keep value numbers in sync
    Unknown        = 0, ///< Not a known command.
    Abort          = 1, ///< Abort.
    CameraOn       = 2, ///< Turn the camera on.
    CameraOff      = 3, ///< Turn the camera off.
    StartRecording = 4, ///< Start recording.
    StopRecording  = 5, ///< Stop recording.
    SdLogOn        = 6, ///< Turn logging to the SD card on.
    SdLogOff       = 7, ///< Turn logging to the SD card off.
};

/** @brief A command message. */
struct CommandMsg
{
    CommandType Command; ///< The command.
};

/** @brief A debug text message. */
struct DebugMsg
{
    uint32_t Status; ///< A status code that goes with the text, defined by the sender.
    // When received from the radio this is only valid for the duration of the callback.
    std::string_view Text; ///< The text. When received from the radio it is valid only during the callback.
};

/** @brief A kind of transport. Mirrors the protobuf Transport. */
enum class TransportKind : uint8_t
{
    // NOTE: mirrors Proto.Transport, keep value numbers in sync
    None = 0, ///< No transport.
    Lora = 1, ///< LoRa.
    Ble  = 2, ///< Bluetooth Low Energy.
    Nrf  = 3, ///< nRF.

    // Not a transport, and never on air (outside Proto.Transport): in a route's preference list it
    // stands for every attached transport, in the order they were added
    /** @brief Not a transport, and never on air: in a route's preference list, every attached transport. */
    Any = 0xFF,
};
} // namespace ra::type
