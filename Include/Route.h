/**
 * @file Route.h
 * @brief What the application sends, and how each kind of message travels.
 *
 * Message is the variant the application hands to Radio::Send() and receives in
 * IRadioListener::OnMessage(); MessageKind indexes its alternatives. A Route says, per kind,
 * whether the message is sent once (best effort) or retried until acknowledged (reliable), which
 * transports it may use and in what order, and whether it waits for an idle transport. Routes are
 * set in RadioConfig (Config.h) and can be overridden per Send().
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

#include "Transport/Transport.h"
#include "Type.h"

namespace ra::turtleford
{
/** @brief Everything the application can send or receive. */
using Message = std::variant<type::FlightData, type::StatusMsg, type::CommandMsg, type::DebugMsg>;

/** @brief Index of each alternative in Message. */
enum class MessageKind : uint8_t
{
    FlightData = 0, ///< type::FlightData: telemetry.
    Status,         ///< type::StatusMsg: the flight state.
    Command,        ///< type::CommandMsg: a command for the other end.
    Debug,          ///< type::DebugMsg: a status code and text.
    Count,          ///< Not a kind: the number of kinds, for arrays indexed by MessageKind.
};
// Routes are looked up by Message::index(), so the two lists must stay in the same order
static_assert(std::is_same_v<std::variant_alternative_t<static_cast<size_t>(MessageKind::Debug), Message>, type::DebugMsg> &&
              std::is_same_v<std::variant_alternative_t<static_cast<size_t>(MessageKind::Command), Message>, type::CommandMsg> &&
              std::is_same_v<std::variant_alternative_t<static_cast<size_t>(MessageKind::Status), Message>, type::StatusMsg> &&
              std::is_same_v<std::variant_alternative_t<static_cast<size_t>(MessageKind::FlightData), Message>, type::FlightData> &&
              std::variant_size_v<Message> == static_cast<size_t>(MessageKind::Count));

/** @brief Whether a message is sent once or retried until acknowledged. */
enum class Delivery : uint8_t
{
    BestEffort, ///< Sent once, may be lost, may arrive out of order.
    /**
     * @brief Retried until acknowledged or given up on (IRadioListener::OnFailed()); delivered once, in order,
     * skipping any that were given up on.
     */
    Reliable,
};

/** @brief How one kind of message travels. */
struct Route
{
    Delivery Mode = Delivery::BestEffort; ///< Sent once, or retried until acknowledged.
    /**
     * @brief Transports the message may use, most wanted first; the list stops at the first None.
     * TransportKind::Any stands for every attached transport, in the order they were added, so a
     * trailing Any means "fall back to anything else": {Ble, Any} prefers BLE and falls back to the
     * rest, {Ble} uses BLE only. The default {Any} uses whichever is available. Entries after Any
     * add nothing. A list with no transport in it allows none, and Send() refuses with NoRoute.
     */
    std::array<TransportKind, 3> Preference {TransportKind::Any};
    /**
     * @brief Hold messages of this kind in the Radio and hand each to a transport only when that
     * transport has nothing queued or in flight, so reliable messages and acks always go first.
     *
     * Up to config::radio::HeldPerKind messages of one kind wait, and go out in the order they were sent.
     * Send() returns Full once that many are waiting: the newest is refused and the application
     * decides what to do with it. Nothing already accepted is dropped or replaced.
     *
     * Applies to Delivery::BestEffort only; ignored on a reliable route. Also ignored for Debug
     * text that is split into several parts: those parts are sent at once or refused whole.
     */
    bool WhenIdle = false;
};

/** @brief Where the internal channel change request may go. It always travels reliably. */
struct ChannelRequestRoute
{
    /** @brief As Route::Preference. */
    std::array<TransportKind, 3> Preference {TransportKind::Any};
};
} // namespace ra::turtleford
