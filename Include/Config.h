/**
 * @file Config.h
 * @brief Every tunable value of the stack, in one place. Change a value here to change it everywhere.
 *
 * One namespace per class that uses the values, under ra::turtleford::config:
 * @verbatim
 *   config::arq         ArqSender, ArqReceiver   (Link/ArqSender.h, Link/ArqReceiver.h)
 *   config::link        Link                     (Link/Link.h)
 *   config::lora        LoraTransport, TurnTaking (Transport/LoraTransport.h, Transport/TurnTaking.h)
 *   config::radio       Radio                    (Radio.h)
 *   config::turtleford  Turtleford               (Turtleford.h)
 *   config::logger      ra::Logger               (Log.h)
 * @endverbatim
 *
 * Two kinds of value:
 *   - Defaults of the run-time config structs (ArqSenderConfig, LinkConfig, LoraTransportConfig,
 *     RadioConfig, TurtlefordConfig, ...). Each struct field takes its default from here; what the
 *     field means is documented on the field. An application can still set any field when it
 *     builds the object.
 *   - Compile-time values: capacities and buffer sizes (memory is allocated once, to these sizes)
 *     and internal timings. Only changeable here.
 *
 * Not here, because the protocol or the hardware sets them: LoraTransport::MaxFrameSize (the radio
 * driver), ArqReceiver::MaxWindow (width of the ack bitmap), the fragment header layout
 * (Transport/Fragment.h), the epoch field width (Radio.cpp) and link::MaxCandidates (Link.h).
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Route.h"
#include "Transport/Transport.h"
#include "WorkQueue.h"

/** @brief The tunable values of the stack, one nested namespace per class. See the file comment. */
namespace ra::turtleford::config
{
/** @brief Values of ArqSender and ArqReceiver (Link/ArqSender.h, Link/ArqReceiver.h). */
namespace arq
{
/**
 * @brief Reliable messages the sender can hold (on air plus waiting behind the window). Compile
 * time: the room allocated in Radio's ArqSender. Also the default and the upper limit of
 * ArqSenderConfig::Capacity.
 */
inline constexpr uint16_t SenderCapacity = 16;
/** @brief Default of ArqSenderConfig::Window. */
inline constexpr uint16_t SenderWindow = 4;
/** @brief Default of ArqSenderConfig::MaxAttempts. */
inline constexpr uint8_t MaxAttempts = 6;
/** @brief Default of ArqSenderConfig::MaxBackoffShift. */
inline constexpr uint8_t MaxBackoffShift = 3;
/** @brief Default of ArqReceiverConfig::Window. 1 to ArqReceiver::MaxWindow. */
inline constexpr uint16_t ReceiverWindow = 8;
} // namespace arq

/** @brief Values of Link (Link/Link.h). */
namespace link
{
/** @brief Default of LinkConfig::LostTimeoutMs. */
inline constexpr uint32_t LostTimeoutMs = 10000;
/** @brief Default of LinkConfig::ConfirmTimeoutMs. */
inline constexpr uint32_t ConfirmTimeoutMs = 4000;
/** @brief Default of LinkConfig::SearchDwellMs. */
inline constexpr uint32_t SearchDwellMs = 2000;
} // namespace link

/** @brief Values of LoraTransport and TurnTaking (Transport/LoraTransport.h, Transport/TurnTaking.h). */
namespace lora
{
/** @brief Default of LoraTransportConfig::ChannelsMHz, the channel table. Both ends must use the same one. */
inline constexpr std::array<float, 1> ChannelsMHz {915.0f};
/** @brief Default of LoraTransportConfig::InitialChannel. */
inline constexpr ChannelNumber InitialChannel = 0;
/** @brief Default of LoraTransportConfig::Address. */
inline constexpr uint8_t Address = 1;
/** @brief Default of LoraTransportConfig::PeerAddress. */
inline constexpr uint8_t PeerAddress = 2;
/** @brief Default of LoraTransportConfig::FrameSize. */
inline constexpr uint16_t FrameSize = 200;
/** @brief Default of LoraTransportConfig::MaxMessageSize. */
inline constexpr uint16_t MaxMessageSize = 512;
/** @brief Default of LoraTransportConfig::RoundTripMs. */
inline constexpr uint32_t RoundTripMs = 800;
/** @brief Default of LoraTransportConfig::ReassemblyTimeoutMs. */
inline constexpr uint32_t ReassemblyTimeoutMs = 3000;
/** @brief Default of LoraTransportConfig::ListenEveryMs. */
inline constexpr uint32_t ListenEveryMs = 1000;
/** @brief Default of LoraTransportConfig::ListenMs. */
inline constexpr uint32_t ListenMs = 150;
/** @brief Default of LoraTransportConfig::ListenGuardMs. */
inline constexpr uint32_t ListenGuardMs = 10;

/** @brief Messages that can wait for the radio at once; LoraTransport::Send() fails beyond this. Compile time. */
inline constexpr uint32_t TxQueueDepth = 16;
/** @brief Frames pulled from the radio per LoraTransport::Receive() call before giving control back. */
inline constexpr int FramesPerReceive = 8;
/** @brief Each listen window is ListenMs plus up to ListenMs / ListenJitterDivisor at random. */
inline constexpr uint32_t ListenJitterDivisor = 4;
/** @brief After the peer's over on a bulk message we may send for ListenMs / StreamSlotDivisor. */
inline constexpr uint32_t StreamSlotDivisor = 2;
} // namespace lora

/** @brief Values of Radio (Radio.h). */
namespace radio
{
/** @brief Default of RadioConfig::Routes: how each message kind travels, indexed by MessageKind. */
inline constexpr std::array<Route, static_cast<size_t>(MessageKind::Count)> Routes {
    Route {.Mode = Delivery::BestEffort, .WhenIdle = true}, // FlightData
    Route {.Mode = Delivery::Reliable},   // Status: the flight state is sent once per change
    Route {.Mode = Delivery::Reliable},   // Command
    Route {.Mode = Delivery::BestEffort}, // Debug
};
/** @brief Default of RadioConfig::ChannelRoute. */
inline constexpr ChannelRequestRoute ChannelRoute {};
/** @brief Default of RadioConfig::ChannelRequestRetries. */
inline constexpr uint8_t ChannelRequestRetries = 2;
/** @brief Default of RadioConfig::TxStallMs. */
inline constexpr uint32_t TxStallMs = 5000;
/** @brief Default of RadioConfig::AckDelayMs. */
inline constexpr uint32_t AckDelayMs = 50;
/** @brief Default of RadioConfig::KeepaliveMs. */
inline constexpr uint32_t KeepaliveMs = 2000;

/** @brief Messages of one kind that can wait at once on a WhenIdle route (Route::WhenIdle). Compile time. */
inline constexpr uint32_t HeldPerKind = 4;
/** @brief Long debug texts sent reliably that can be on their way at once. Compile time. */
inline constexpr uint32_t MaxReliableTexts = 4;
/** @brief Messages taken from one transport per Radio::Poll() before moving on to the next. */
inline constexpr int ReceivesPerPoll = 8;
/** @brief Smallest gap between two frames sent only to tell the peer our epoch. */
inline constexpr uint32_t EpochSyncIntervalMs = 200;
/** @brief Each keepalive interval is KeepaliveMs plus up to KeepaliveMs / KeepaliveJitterDivisor at random. */
inline constexpr uint32_t KeepaliveJitterDivisor = 4;
/** @brief Each retransmit timeout is the round trip plus up to round trip / RetransmitJitterDivisor at random. */
inline constexpr uint32_t RetransmitJitterDivisor = 2;
} // namespace radio

/** @brief Values of Turtleford (Turtleford.h). */
namespace turtleford
{
/** @brief Default of TurtlefordConfig::PollIntervalMs. */
inline constexpr uint32_t PollIntervalMs = 5;
/** @brief Default of TurtlefordConfig::PollPriority. */
inline constexpr hal::WorkQueue::Priority PollPriority = hal::WorkQueue::Priority::High;
/** @brief Default of TurtlefordConfig::LogEvents. */
inline constexpr bool LogEvents = true;
/**
 * @brief How long the Turtleford destructor waits for the work queue to let go of the poll job. A
 * queue whose scheduler is not running never lets go, and nothing can run the job then, so giving
 * up is safe in that case. Only used with a preemptive work queue (WORK_QUEUE_PREEMPTIVE).
 */
inline constexpr uint32_t JobReleaseTimeoutMs = 1000;
} // namespace turtleford

/** @brief Values of ra::Logger (Log.h). */
namespace logger
{
/** @brief Bytes of one log entry's encode buffer in ra::Logger; an entry that does not fit is not logged. */
inline constexpr size_t BufferSize = 256;
/** @brief Bytes ra::Logger caches before they are handed to the storage writer in one piece. */
inline constexpr size_t CacheSize = 1024;
} // namespace logger
} // namespace ra::turtleford::config
