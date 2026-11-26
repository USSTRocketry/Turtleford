#pragma once

// Matchers for what a Radio hands to its application, in two flavours:
//
//   - on a sim::Recorder::Received (the copy a Recorder keeps), for checking afterwards:
//         EXPECT_THAT(A.Events.Messages, ElementsAre(IsCommand(CommandType::CameraOn)));
//   - on the Message variant itself (MessageIs...), for EXPECT_CALL on MockRadioListener::OnMessage:
//         EXPECT_CALL(Listener, OnMessage(MessageIsCommand(CommandType::Abort), _));

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gmock/gmock.h>

#include "Radio.h"
#include "Sim/Recorder.h"

namespace mocks
{
// ---- on Recorder::Received ----------------------------------------------------

MATCHER_P(IsKind, Kind, std::string(negation ? "is not" : "is") + " a " + testing::PrintToString(Kind))
{
    return arg.Kind == Kind;
}

MATCHER_P(ArrivedVia, Via, std::string(negation ? "did not arrive" : "arrived") + " via " + testing::PrintToString(Via))
{
    return arg.Info.Via == Via;
}

MATCHER(ArrivedReliably, std::string(negation ? "did not arrive" : "arrived") + " reliably")
{
    return arg.Info.Reliable;
}

MATCHER_P(HasTimestamp, Ms, std::string(negation ? "was not" : "was") + " stamped " + testing::PrintToString(Ms) + " ms")
{
    return arg.Info.Timestamp == static_cast<uint32_t>(Ms);
}

MATCHER_P(HasChunksLeft, Count, std::string(negation ? "does not have" : "has") + " " + testing::PrintToString(Count) + " chunks left")
{
    return arg.Info.ChunksLeft == static_cast<uint32_t>(Count);
}

/** @brief A Command message carrying Command. */
MATCHER_P(IsCommand, Command, std::string(negation ? "is not" : "is") + " a Command message carrying " + testing::PrintToString(Command))
{
    return arg.Kind == ra::turtleford::MessageKind::Command && arg.Command == Command;
}

/** @brief A Status message carrying flight state State (std::nullopt for none). */
MATCHER_P(IsFlightState, State, std::string(negation ? "is not" : "is") + " a Status message carrying flight state " + testing::PrintToString(State))
{
    return arg.Kind == ra::turtleford::MessageKind::Status && arg.State == State;
}

/** @brief A Debug message with this status and text. */
MATCHER_P2(IsDebug, Status, Text, std::string(negation ? "is not" : "is") + " a Debug message with status " + testing::PrintToString(Status) + " and text " + testing::PrintToString(Text))
{
    return arg.Kind == ra::turtleford::MessageKind::Debug && arg.DebugStatus == static_cast<uint32_t>(Status) &&
           arg.DebugText == std::string {Text};
}

/** @brief A FlightData message whose every field equals Expected. */
MATCHER_P(CarriesFlightData, Expected, "carries the expected flight data")
{
    if (arg.Kind != ra::turtleford::MessageKind::FlightData)
    {
        *result_listener << "is not a FlightData message";
        return false;
    }

    bool Ok          = true;
    const auto Check = [&](const char* Name, float Got, float Want)
    {
        if (!testing::Matches(testing::FloatEq(Want))(Got))
        {
            *result_listener << Name << " is " << Got << ", expected " << Want << "; ";
            Ok = false;
        }
    };

    const auto& G = arg.Flight;
    const auto& E = Expected;
    Check("BMP temperature", G.BMP_Data.Temperature, E.BMP_Data.Temperature);
    Check("BMP pressure", G.BMP_Data.Pressure, E.BMP_Data.Pressure);
    Check("BMP altitude", G.BMP_Data.Altitude, E.BMP_Data.Altitude);
    Check("accel/gyro temperature", G.AccelGyroTemperature, E.AccelGyroTemperature);
    Check("accel X", G.Accel.X, E.Accel.X);
    Check("accel Y", G.Accel.Y, E.Accel.Y);
    Check("accel Z", G.Accel.Z, E.Accel.Z);
    Check("gyro X", G.Gyro.X, E.Gyro.X);
    Check("gyro Y", G.Gyro.Y, E.Gyro.Y);
    Check("gyro Z", G.Gyro.Z, E.Gyro.Z);
    Check("magnetometer X", G.Magnetometer.X, E.Magnetometer.X);
    Check("magnetometer Y", G.Magnetometer.Y, E.Magnetometer.Y);
    Check("magnetometer Z", G.Magnetometer.Z, E.Magnetometer.Z);
    Check("thermometer", G.Thermometer, E.Thermometer);
    Check("GPS latitude", G.GPS_Data.Latitude, E.GPS_Data.Latitude);
    Check("GPS longitude", G.GPS_Data.Longitude, E.GPS_Data.Longitude);
    Check("GPS altitude", G.GPS_Data.Altitude, E.GPS_Data.Altitude);
    Check("GPS speed", G.GPS_Data.Speed, E.GPS_Data.Speed);
    Check("GPS angle", G.GPS_Data.Angle, E.GPS_Data.Angle);
    if (G.GPS_Data.FixQuality != E.GPS_Data.FixQuality || G.GPS_Data.Satellites != E.GPS_Data.Satellites)
    {
        *result_listener << "GPS fix " << G.GPS_Data.FixQuality << " with " << G.GPS_Data.Satellites << " satellites; ";
        Ok = false;
    }
    return Ok;
}

// ---- text that was split into parts ---------------------------------------------

/** @brief The text of every part, in the order the parts arrived. */
inline std::string JoinedText(const std::vector<sim::Recorder::Received>& Parts)
{
    std::string Joined;
    for (const auto& Part : Parts) { Joined += Part.DebugText; }
    return Joined;
}

/**
 * @brief What each part of a text that was split into Count parts should look like: a Debug
 * message with this status, and the number of parts still to come counting down to 0.
 */
inline std::vector<testing::Matcher<const sim::Recorder::Received&>> PartsCountingDown(uint32_t Status, size_t Count)
{
    std::vector<testing::Matcher<const sim::Recorder::Received&>> Parts;
    for (size_t i = 0; i < Count; ++i)
    {
        Parts.push_back(testing::AllOf(IsKind(ra::turtleford::MessageKind::Debug),
                                       testing::Field(&sim::Recorder::Received::DebugStatus, Status),
                                       HasChunksLeft(Count - 1 - i)));
    }
    return Parts;
}

// ---- on the Message variant ---------------------------------------------------

MATCHER_P(MessageIsCommand,
          Command,
          std::string(negation ? "is not" : "is") + " a Command message carrying " + testing::PrintToString(Command))
{
    const auto* Got = std::get_if<ra::type::CommandMsg>(&arg);
    return Got != nullptr && Got->Command == Command;
}
} // namespace mocks
