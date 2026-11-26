#pragma once

// A listener that remembers everything a Radio reports, for tests that check what arrived
// afterwards. Mocks/MockRadioListener.h delegates to one so the same object can also carry
// call expectations.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Radio.h"

namespace sim
{
using namespace ra::turtleford;

/** @brief Records everything a Radio reports. */
class Recorder final : public IRadioListener
{
public:
    struct Received
    {
        MessageKind Kind;
        RxInfo Info;
        ra::type::CommandType Command {};         // Kind == Command
        ra::type::FlightData Flight {};           // Kind == FlightData
        std::optional<ra::type::FlightState> State {}; // Kind == Status
        uint32_t DebugStatus = 0;                 // Kind == Debug
        std::string DebugText {};
    };

    std::vector<Received> Messages;
    std::vector<Ticket> DeliveredTickets;
    std::vector<Ticket> FailedTickets;
    std::vector<std::pair<TransportKind, bool>> LinkChanges;
    std::vector<std::pair<TransportKind, ChannelNumber>> ChannelChanges;
    std::vector<TransportKind> PeerRestarts;

    /** @brief Runs inside OnFailed, lets a test resend from the callback. */
    std::function<void(Ticket)> OnFailedHook {};

    std::vector<ra::type::CommandType> Commands() const
    {
        std::vector<ra::type::CommandType> Out;
        for (const auto& M : Messages)
        {
            if (M.Kind == MessageKind::Command) { Out.push_back(M.Command); }
        }
        return Out;
    }

    size_t Count(MessageKind Kind) const
    {
        size_t N = 0;
        for (const auto& M : Messages) { N += (M.Kind == Kind) ? 1 : 0; }
        return N;
    }

    void OnMessage(const Message& Msg, const RxInfo& Info) override
    {
        Received R {.Kind = static_cast<MessageKind>(Msg.index()), .Info = Info};

        if (const auto* Command = std::get_if<ra::type::CommandMsg>(&Msg)) { R.Command = Command->Command; }
        if (const auto* Flight = std::get_if<ra::type::FlightData>(&Msg)) { R.Flight = *Flight; }
        if (const auto* Control = std::get_if<ra::type::StatusMsg>(&Msg)) { R.State = Control->State; }
        if (const auto* Debug = std::get_if<ra::type::DebugMsg>(&Msg))
        {
            R.DebugStatus = Debug->Status;
            R.DebugText   = std::string {Debug->Text};
        }

        Messages.push_back(std::move(R));
    }

    void OnDelivered(Ticket Id) override { DeliveredTickets.push_back(Id); }
    void OnFailed(Ticket Id) override
    {
        FailedTickets.push_back(Id);
        if (OnFailedHook) { OnFailedHook(Id); }
    }
    void OnLinkChanged(TransportKind Kind, bool Up) override { LinkChanges.emplace_back(Kind, Up); }
    void OnChannelChanged(TransportKind Kind, ChannelNumber Channel) override { ChannelChanges.emplace_back(Kind, Channel); }
    void OnPeerRestarted(TransportKind Kind) override { PeerRestarts.push_back(Kind); }
};
} // namespace sim

// Names in failure messages instead of bare numbers
namespace ra::type
{
inline void PrintTo(CommandType Command, std::ostream* Out)
{
    static constexpr const char* Names[] = {
        "Unknown", "Abort", "CameraOn", "CameraOff", "StartRecording", "StopRecording", "SdLogOn", "SdLogOff"};
    const auto Index = static_cast<size_t>(Command);
    *Out << "CommandType::" << (Index < std::size(Names) ? Names[Index] : "?");
}

inline void PrintTo(FlightState State, std::ostream* Out)
{
    static constexpr const char* Names[] = {"Unknown", "Unarmed", "GroundIdle", "InFlight", "MainChute"};
    const auto Index = static_cast<size_t>(State);
    *Out << "FlightState::" << (Index < std::size(Names) ? Names[Index] : "?");
}

inline void PrintTo(TransportKind Kind, std::ostream* Out)
{
    static constexpr const char* Names[] = {"None", "Lora", "Ble", "Nrf"};
    const auto Index = static_cast<size_t>(Kind);
    *Out << "TransportKind::" << (Index < std::size(Names) ? Names[Index] : "?");
}
} // namespace ra::type

namespace ra::turtleford
{
inline void PrintTo(MessageKind Kind, std::ostream* Out)
{
    static constexpr const char* Names[] = {"FlightData", "Status", "Command", "Debug"};
    const auto Index = static_cast<size_t>(Kind);
    *Out << "MessageKind::" << (Index < std::size(Names) ? Names[Index] : "?");
}
} // namespace ra::turtleford

namespace sim
{
inline void PrintTo(const Recorder::Received& Got, std::ostream* Out)
{
    *Out << "{" << testing::PrintToString(Got.Kind) << " via " << testing::PrintToString(Got.Info.Via)
         << (Got.Info.Reliable ? ", reliable" : ", best effort") << ", chunks left " << Got.Info.ChunksLeft;
    if (Got.Kind == MessageKind::Command) { *Out << ", " << testing::PrintToString(Got.Command); }
    if (Got.Kind == MessageKind::Status)
    {
        *Out << ", " << (Got.State ? testing::PrintToString(*Got.State) : std::string("no state"));
    }
    if (Got.Kind == MessageKind::Debug) { *Out << ", status " << Got.DebugStatus << ", \"" << Got.DebugText << "\""; }
    *Out << "}";
}
} // namespace sim
