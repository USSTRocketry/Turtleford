/**
 * @file Transport.h
 * @brief The one interface every radio type implements.
 *
 * A transport moves whole messages to the single peer on the other end. It hides everything
 * that is specific to the radio: frame size, chunking and reassembly, channel to frequency
 * mapping. It does NOT retry, order or deduplicate; that is done once, above all transports,
 * by Radio.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Type.h"

namespace ra::turtleford
{
/** @brief Which radio type a transport is. See type::TransportKind. */
using TransportKind = type::TransportKind;

/**
 * @brief A channel of one transport. What a number means is up to the transport (for LoRa it is an
 * index into LoraTransportConfig::ChannelsMHz); both ends of a link must agree on it. Same width as
 * the channel field of the SwitchChannel message.
 */
using ChannelNumber = uint32_t;

/** @brief What a transport can do, as ITransport::Caps() reports it. */
struct TransportCaps
{
    /** @brief Which radio type this is. */
    TransportKind Kind = TransportKind::None;
    /** @brief Largest message Send() accepts, in bytes. Larger than one frame means the transport chunks. */
    uint16_t MaxMessageSize = 0;
    /**
     * @brief Rough time in ms for a message to go out and its ack to come back.
     * Used as the base retransmit timeout for messages sent on this transport.
     */
    uint32_t RoundTripMs = 0;
    /** @brief Number of channels SetChannel() accepts. 0 or 1 means no channel control. */
    ChannelNumber ChannelCount = 0;
};

/**
 * @brief A radio that carries whole messages to one peer. Implemented by LoraTransport.
 *
 * Nothing here blocks beyond one frame: Radio calls Poll() and Receive() from its own poll.
 */
class ITransport
{
public:
    virtual ~ITransport() = default;

    /** @brief What this transport can do. */
    virtual const TransportCaps& Caps() const = 0;

    /** @brief False while the radio physically cannot carry traffic (e.g. BLE not connected). */
    virtual bool IsReady() const = 0;

    /**
     * @brief Queue one whole message for transmission.
     * @param Message The bytes to send. Copied; the caller keeps ownership.
     * @param Bulk One of a continuous stream (telemetry) with more right behind it and no answer
     *             expected. A half duplex transport uses this to decide when to stop and listen;
     *             others ignore it.
     * @return False if the message is too large or there is no room; nothing is queued then.
     */
    virtual bool Send(std::span<const std::byte> Message, bool Bulk = false) = 0;

    /** @brief How many more messages Send() is certain to accept right now. */
    virtual size_t TxFree() const = 0;

    /**
     * @brief Running count of messages accepted by Send() that are finished with: fully sent,
     * or given up on. Wraps. The Nth accepted message has left once this reaches N, which is
     * how Radio knows when to start waiting for its acknowledgement.
     */
    virtual uint32_t TxCompleted() const = 0;

    /**
     * @brief True when everything accepted by Send() has physically left the radio and a message
     * handed over now would go out on the next Poll(), instead of waiting for a turn to speak.
     * Radio hands WhenIdle messages (telemetry) over only while this is true, so while a transport
     * holds back they wait in Radio, and reliable frames and acks go first.
     */
    virtual bool TxIdle() const = 0;

    /**
     * @brief Pump the radio: push queued frames out. Must not block longer than one frame.
     * @param NowMs Current time.
     */
    virtual void Poll(uint32_t NowMs) = 0;

    /**
     * @brief Fetch the next whole received message.
     * @param NowMs Current time.
     * @return The message, or an empty span when there is none. The span stays valid until
     *         the next call to Receive() or Poll().
     */
    virtual std::span<const std::byte> Receive(uint32_t NowMs) = 0;

    /**
     * @brief Retune immediately. Callers are responsible for waiting on TxIdle() first.
     * @param Channel One of 0 .. TransportCaps::ChannelCount - 1.
     * @return False if the channel is not one this transport has; nothing changes then.
     */
    virtual bool SetChannel(ChannelNumber Channel) = 0;
    /** @brief The channel the transport is on now. */
    virtual ChannelNumber Channel() const          = 0;
};
} // namespace ra::turtleford
