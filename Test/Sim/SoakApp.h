#pragma once

// An application that acts at random, for the soak tests, and the checks of what the stack
// promised it. It is attached to a Radio it does not own, and its callbacks are called by whatever
// listener the test gives that Radio.
//
// What it does: sends reliable messages (DebugMsg, Status = a number unique to this end), best effort
// chatter and telemetry; answers a message from inside OnMessage; sends the next reliable message
// from OnDelivered and OnFailed; and, on the ground, asks for channel changes at random and again
// from OnChannelChanged. Quiet stops all of it, from the loop and the callbacks.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Radio.h"
#include "Random.h"

namespace sim
{
class SoakApp
{
public:
    SoakApp(uint32_t Seed, bool Master) : m_Rng(Seed), m_Master(Master) {}

    void Bind(ra::turtleford::Radio& R) { m_R = &R; }

    // ---- what the application does on its own, once per millisecond --------------
    void Act()
    {
        using namespace ra::type;
        if (Quiet) { return; }
        if (Chance(2, 1000)) { SendReliable(); }
        if (Chance(10, 1000)) { m_R->Send(DebugMsg {.Status = 0, .Text = "chatter"}, BestEffort()); }
        if (Chance(20, 1000)) { m_R->Send(Sample()); }
        if (m_Master && Chance(2, 10000)) { m_R->RequestChannel(ra::turtleford::TransportKind::Lora, RandomChannel()); }
    }

    // Telemetry with every field set, so it is as large on air as a real sample (zeros encode to nothing)
    ra::type::FlightData Sample()
    {
        const float V = static_cast<float>(m_Rng.Value(1, 1000)) + 0.5f;
        ra::type::FlightData Data {};
        Data.BMP_Data             = {.Temperature = V, .Pressure = V + 1, .Altitude = V + 2};
        Data.AccelGyroTemperature = V + 3;
        Data.Accel                = {.X = V + 4, .Y = V + 5, .Z = V + 6};
        Data.Gyro                 = {.X = V + 7, .Y = V + 8, .Z = V + 9};
        Data.Magnetometer         = {.X = V + 10, .Y = V + 11, .Z = V + 12};
        Data.Thermometer          = V + 13;
        Data.GPS_Data             = {.Latitude   = V + 14,
                                     .Longitude  = V + 15,
                                     .Altitude   = V + 16,
                                     .Speed      = V + 17,
                                     .Angle      = V + 18,
                                     .FixQuality = 1,
                                     .Satellites = 9};
        return Data;
    }

    void SendReliable()
    {
        const uint32_t Id = m_NextId;
        const auto Result = m_R->Send(ra::type::DebugMsg {.Status = Id, .Text = ""}, Reliable());
        if (Result.Status != ra::turtleford::SendStatus::Queued) { return; } // full: try again later

        ++m_NextId;
        SentId[Result.Id] = Id;
    }

    // ---- what the application does when told something ----------------------------
    void OnMessage(const ra::turtleford::Message& Msg, const ra::turtleford::RxInfo& Info)
    {
        const auto* Debug = std::get_if<ra::type::DebugMsg>(&Msg);
        if (Debug != nullptr && Info.Reliable) { ReceivedIds.push_back(Debug->Status); }

        // answer from inside the callback, best effort (goes out at once) or reliable
        if (Quiet) { return; }
        if (Chance(30, 100)) { m_R->Send(ra::type::DebugMsg {.Status = 0, .Text = "re"}, BestEffort()); }
        if (Chance(10, 100)) { SendReliable(); }
    }

    void OnDelivered(ra::turtleford::Ticket Id)
    {
        Report(Id, Delivered);
        if (!Quiet && Chance(20, 100)) { SendReliable(); }
    }

    void OnFailed(ra::turtleford::Ticket Id)
    {
        Report(Id, Failed);
        if (!Quiet && Chance(50, 100)) { SendReliable(); } // the application's own retry policy
    }

    void OnChannelChanged(ra::turtleford::TransportKind Kind, ra::turtleford::ChannelNumber Channel)
    {
        if (Kind != ra::turtleford::TransportKind::Lora) { return; }
        ChannelReports.push_back(Channel);
        if (!Quiet && m_Master && Chance(30, 100)) { m_R->RequestChannel(Kind, RandomChannel()); }
    }

    /** @brief The application has stopped sending anything, from its loop or its callbacks. */
    bool Quiet = false;

    // ---- what the checks look at ----------------------------------------------------
    std::map<ra::turtleford::Ticket, uint32_t> SentId; // every accepted reliable message, by its ticket
    std::vector<uint32_t> ReceivedIds;                 // reliable messages from the other end, in arrival order
    std::set<ra::turtleford::Ticket> Delivered;
    std::set<ra::turtleford::Ticket> Failed;
    std::vector<ra::turtleford::ChannelNumber> ChannelReports;               // LoRa channels reported by OnChannelChanged
    std::vector<std::string> Problems;                 // reports that make no sense on their own

    /**
     * @brief Everything From sent reliably, against what To received: every message has exactly one
     * outcome, OnDelivered only for one that arrived, and what arrived came once each and in order
     * (messages given up on are skipped, never delivered twice).
     */
    static void CheckDirection(const char* Name, const SoakApp& From, const SoakApp& To)
    {
        SCOPED_TRACE(Name);
        EXPECT_THAT(From.Problems, testing::IsEmpty());

        const std::set<uint32_t> Arrived(To.ReceivedIds.begin(), To.ReceivedIds.end());
        size_t Unresolved = 0;
        for (const auto& [Id, Payload] : From.SentId)
        {
            Unresolved += (From.Delivered.contains(Id) || From.Failed.contains(Id)) ? 0 : 1;
            if (From.Delivered.contains(Id))
            {
                EXPECT_TRUE(Arrived.contains(Payload)) << "reported delivered but never received: message " << Payload;
            }
        }
        EXPECT_EQ(Unresolved, 0u) << "accepted reliable messages with no outcome";

        const auto NotRising = std::adjacent_find(To.ReceivedIds.begin(),
                                                  To.ReceivedIds.end(),
                                                  [](uint32_t Earlier, uint32_t Later) { return Later <= Earlier; });
        EXPECT_TRUE(NotRising == To.ReceivedIds.end())
            << "reliable messages must arrive once each, in the order they were sent; message " << *NotRising
            << " was followed by " << *std::next(NotRising);
    }

private:
    static ra::turtleford::Route Reliable() { return {.Mode = ra::turtleford::Delivery::Reliable}; }
    static ra::turtleford::Route BestEffort() { return {.Mode = ra::turtleford::Delivery::BestEffort}; }

    void Report(ra::turtleford::Ticket Id, std::set<ra::turtleford::Ticket>& Into)
    {
        if (!SentId.contains(Id)) { Problems.push_back("report for ticket " + std::to_string(Id) + " never handed out"); }
        if (Delivered.contains(Id) || Failed.contains(Id)) { Problems.push_back("second report for ticket " + std::to_string(Id)); }
        Into.insert(Id);
    }

    bool Chance(uint32_t Times, uint32_t Of) { return m_Rng.Value<uint32_t>(0, Of - 1) < Times; }
    ra::turtleford::ChannelNumber RandomChannel() { return m_Rng.Value<ra::turtleford::ChannelNumber>(0, 3); }

    ra::turtleford::Radio* m_R = nullptr;
    ra::RNG m_Rng;
    bool m_Master;
    uint32_t m_NextId = 1;
};
} // namespace sim
