#pragma once

// Sample messages shared by the end to end tests.

#include <cstddef>
#include <string>

#include "Radio.h"
#include "Type.h"

namespace sim
{
inline ra::type::FlightData SampleFlightData()
{
    ra::type::FlightData Data {};
    Data.BMP_Data             = {.Temperature = 21.5f, .Pressure = 1013.25f, .Altitude = 512.0f};
    Data.AccelGyroTemperature = 30.0f;
    Data.Accel                = {.X = 1.0f, .Y = 2.0f, .Z = 3.0f};
    Data.Gyro                 = {.X = 4.0f, .Y = 5.0f, .Z = 6.0f};
    Data.Magnetometer         = {.X = 7.0f, .Y = 8.0f, .Z = 9.0f};
    Data.Thermometer          = 19.0f;
    Data.GPS_Data             = {.Latitude   = 52.13f,
                                 .Longitude  = -106.67f,
                                 .Altitude   = 480.0f,
                                 .Speed      = 12.0f,
                                 .Angle      = 90.0f,
                                 .FixQuality = 1,
                                 .Satellites = 9};
    return Data;
}

// A sample that differs from the others by its altitude, so the one that arrived can be told
inline ra::type::FlightData SampleAt(float Altitude)
{
    ra::type::FlightData Data = SampleFlightData();
    Data.BMP_Data.Altitude    = Altitude;
    return Data;
}

// Telemetry that goes straight into the transport's queue instead of waiting for the transport to be idle
inline const ra::turtleford::Route QueuedTelemetry {.Mode = ra::turtleford::Delivery::BestEffort};

// Text in which every part differs, so a swapped or repeated part shows
inline std::string LongText(size_t Length)
{
    std::string Text(Length, ' ');
    for (size_t i = 0; i < Length; ++i) { Text[i] = static_cast<char>('A' + i % 23); }
    return Text;
}

// Command N of a long, recognisable sequence
inline ra::type::CommandType CommandAt(int Index) { return static_cast<ra::type::CommandType>(1 + (Index % 7)); }
} // namespace sim
