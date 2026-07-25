#pragma once
#include <cstdint>
#include <optional>

namespace ra::type
{
template <typename T>
struct Vector3
{
    T X;
    T Y;
    T Z;
};

struct FlightData
{
    using Vector3f = Vector3<float>;
    struct BMP_Info
    {
        float Temperature;
        float Pressure;
        float Altitude;
    };

    BMP_Info BMP_Data;
    float AccelGyroTemperature;
    Vector3f Accel;
    Vector3f Gyro;
    Vector3f Magnetometer;
    float Thermometer;
};

enum class FlightState : uint32_t
{
    Unknown    = 0,
    Unarmed    = 1,
    GroundIdle = 2,
    InFlight   = 3,
    MainChute  = 4,
};

struct FlightControlMsg
{
    std::optional<FlightState> State;
};

enum class Category : uint32_t
{
    // NOTE: These values are serialized into log history.
    // Never change or reorder existing entries.
    Unknown        = 0,
    Communications = 1,
    Application    = 2,
    Platform       = 3,
    FlightControl  = 4,
    Sensors        = 5,
    Storage        = 6,
    Tasking        = 7,
};

enum class CommandType : uint32_t
{
    Unknown        = 0,
    Abort          = 1,
    CameraOn       = 2,
    CameraOff      = 3,
    StartRecording = 4,
    StopRecording  = 5,
};

struct CommandMsg
{
    CommandType Command;
};
} // namespace ra::type
