#pragma once
#include <cstdint>

namespace ra::turtleford::type
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

    uint32_t TimestampMs;
    BMP_Info BMP_Data;
    float AccelGyroTemperature;
    Vector3f Accel;
    Vector3f Gyro;
    Vector3f Magnetometer;
    float Thermometer;
};

enum class DataType
{
    Invalid,
    FlightData,
    FlightBegin,
};
} // namespace ra::turtleford::type
