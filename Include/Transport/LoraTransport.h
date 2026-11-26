/**
 * @file LoraTransport.h
 * @brief ITransport for a LoRa radio: fragmenting, a transmit queue, and half duplex turn taking.
 *
 * LoraTransport sits on HAL::ITelemetryRadio. Messages are cut into frames by Fragmenter and put back
 * together by Reassembler (Transport/Fragment.h); who may transmit when is decided by TurnTaking
 * (Transport/TurnTaking.h). LoraTransportConfig holds the settings.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "abstractions/ITelemetryRadio.h"

#include "Config.h"
#include "Transport/Fragment.h"
#include "DataStructure/Buffer/PacketQueue.h"
#include "Transport/Transport.h"
#include "Transport/TurnTaking.h"

namespace ra::turtleford
{
/** @brief Settings of a LoraTransport. Defaults are in Config.h, config::lora. */
struct LoraTransportConfig
{
    /** @brief Channel table, frequencies in MHz. Both ends must be built with the same one. Index 0 is the default. */
    std::vector<float> ChannelsMHz = std::vector<float>(config::lora::ChannelsMHz.begin(), config::lora::ChannelsMHz.end());
    /** @brief Index into ChannelsMHz the radio starts on. Index 0 is used instead if this is not in the table. */
    ChannelNumber InitialChannel = config::lora::InitialChannel;

    /**
     * @brief Our own address, fixed per device. Stamped on every frame we send, and the only
     * destination the radio accepts frames for. 255 is the radio's broadcast address: do not use it.
     */
    uint8_t Address = config::lora::Address;
    /**
     * @brief Address of the one device we talk to. Everything is sent to it, and frames from
     * any other sender are ignored. The two ends are configured as mirror images of each other.
     */
    uint8_t PeerAddress = config::lora::PeerAddress;

    /** @brief Bytes per LoRa packet, fragment header included. Capped at LoraTransport::MaxFrameSize. */
    uint16_t FrameSize = config::lora::FrameSize;
    /**
     * @brief Largest message accepted, in bytes; anything above FrameSize is chunked. Lowered to what
     * a Fragmenter of that FrameSize can cut up, if that is less.
     */
    uint16_t MaxMessageSize = config::lora::MaxMessageSize;

    /** @brief Time in ms for one frame out and the ack back at the configured modem settings. */
    uint32_t RoundTripMs = config::lora::RoundTripMs;
    /** @brief A partly received message is dropped if its next fragment takes longer than this many ms. */
    uint32_t ReassemblyTimeoutMs = config::lora::ReassemblyTimeoutMs;

    /**
     * @brief LoRa is half duplex: while we transmit we hear nothing. So whoever has said what it had
     * to say marks its last message "over" and stays off the air for ListenMs, which is when the
     * peer gets a message of its own in (a command, an ack). A sender that never stops (telemetry)
     * says "over" only once per ListenEveryMs. A listener that has not heard "over" waits for it.
     * Costs a sender in a continuous stream ListenMs out of every ListenEveryMs, and a message that
     * has to wait for the peer's "over" up to ListenEveryMs. Costs nothing while messages have gaps
     * between them anyway. ListenEveryMs = 0 turns it all off: messages go out back to back.
     * Unit: ms.
     */
    uint32_t ListenEveryMs = config::lora::ListenEveryMs;
    /**
     * @brief How long in ms we stay off the air after a message marked "over", plus up to
     * ListenMs / config::lora::ListenJitterDivisor at random so two talkers do not keep colliding.
     * About twice the airtime of what the peer needs to get in, plus both ends' poll intervals.
     * Unit: ms.
     */
    uint32_t ListenMs = config::lora::ListenMs;
    /**
     * @brief After the peer's message marked "over" we wait this long before we answer. The peer's
     * radio only listens again once its application calls receive(), up to one poll interval after
     * its frame ended, and a frame that starts before that is lost. At least the peer's poll interval.
     * Unit: ms.
     */
    uint32_t ListenGuardMs = config::lora::ListenGuardMs;
};

/**
 * @brief LoRa transport on top of HAL::ITelemetryRadio.
 *
 * Everything is allocated in the constructor, including room for config::lora::TxQueueDepth waiting
 * messages of MaxMessageSize each. The radio must already be initialised (begin() called,
 * modem parameters set) and must outlive the transport.
 *
 * Turn taking (LoraTransportConfig::ListenEveryMs, ListenMs, ListenGuardMs; the logic is in
 * Transport/TurnTaking.h, this class reports to it what goes out and comes in). The radio cannot hear
 * while it sends, so two ends have to say when they are listening. A message carries a flag
 * in its last fragment:
 *
 *   - "over": the sender has said what it had to say and now stays off the air for ListenMs
 *     (plus up to a quarter more at random). A message is marked over when it is the last one in
 *     the queue, or, if it is bulk (telemetry: one of a stream, more right behind, no answer
 *     expected), once per ListenEveryMs.
 *   - "bulk": the message belongs to a stream. Set from the bulk argument of Send().
 *
 * On the receiving side:
 *
 *   - A message that is not marked over means the peer has more to say: we hold our own messages.
 *   - A message marked over ends our own wait at once, and after ListenGuardMs we are free to send.
 *     If it was also bulk (a slot inside a stream) we are free for ListenMs / 2, then hold again.
 *   - Holding ends by itself when the peer has been silent for a window and half a RoundTripMs (it may
 *     be quiet for that long in the middle of a stream), or when it has not
 *     said "over" for 2 x ListenEveryMs.
 *
 * Fragments of one message always go out on consecutive polls.
 */
class LoraTransport final : public ITransport
{
public:
    /** @brief Largest packet the RFM95 driver carries, in bytes. LoraTransportConfig::FrameSize is capped at this. */
    static constexpr uint16_t MaxFrameSize = 251;

    /**
     * @param Radio  Must be initialised already and must outlive the transport. Its own address, destination
     *               address and frequency are set here, and promiscuous mode is turned off.
     * @param Config Copied. Out of range values are clamped, see the fields of LoraTransportConfig.
     */
    LoraTransport(HAL::ITelemetryRadio& Radio, const LoraTransportConfig& Config = {});

    /** @brief See ITransport::Caps(). MaxMessageSize is the size of a queue slot. */
    const TransportCaps& Caps() const override { return m_Caps; }
    /** @brief See ITransport::IsReady(). Always true: the radio is there from the start. */
    bool IsReady() const override { return true; }

    /**
     * @brief See ITransport::Send(). The message is copied into the transmit queue.
     * @param Bytes The message.
     * @param Bulk  Sets the "bulk" mark on the message, see the class description.
     * @return False if the message is empty, longer than Caps().MaxMessageSize, or the queue is full.
     */
    bool Send(std::span<const std::byte> Bytes, bool Bulk = false) override;
    /** @brief See ITransport::TxFree(). Free places in the transmit queue. */
    size_t TxFree() const override { return m_TxQueue.Free(); }
    /**
     * @brief See ITransport::TxCompleted(). Counts a message once its last frame is handed to the radio,
     * or when it is dropped for being unsendable.
     */
    uint32_t TxCompleted() const override { return m_TxCompleted; }
    /** @brief See ITransport::TxIdle(). The queue is empty, no message is half sent and turn taking allows sending. */
    bool TxIdle() const override { return m_TxQueue.Empty() && m_Fragmenter.Done() && m_Turns.MayTransmit(); }
    /**
     * @brief See ITransport::Poll(). Hands at most one frame to the radio, which blocks until it is off the air.
     * Once a message is started its remaining frames go out on the following polls, one per poll.
     * @param NowMs Current time.
     */
    void Poll(uint32_t NowMs) override;
    /**
     * @brief See ITransport::Receive(). Reads up to config::lora::FramesPerReceive frames from the radio per call.
     * @param NowMs Current time.
     * @return The next complete message, or an empty span when there is none. Valid until the next call
     *         to Receive() or Poll().
     */
    std::span<const std::byte> Receive(uint32_t NowMs) override;

    /**
     * @brief See ITransport::SetChannel(). Tunes the radio to the matching entry of LoraTransportConfig::ChannelsMHz.
     * @param Channel Index into the channel table.
     * @return False if the index is not in the table; nothing changes then.
     */
    bool SetChannel(ChannelNumber Channel) override;
    /** @brief See ITransport::Channel(). */
    ChannelNumber Channel() const override { return m_Channel; }

    /** @brief Received frames that were thrown away (not from the peer, broken or incomplete message). */
    uint32_t DroppedFrames() const { return m_Reassembler.Dropped(); }

private:
    HAL::ITelemetryRadio& m_Radio; ///< The radio. Owned by the caller.
    std::vector<float> m_ChannelsMHz; ///< The channel table, in MHz.
    TransportCaps m_Caps;             ///< What Caps() returns. Filled in by the constructor.

    Fragmenter m_Fragmenter; ///< Cuts the message at the front of m_TxQueue into frames.
    /**
     * @brief Messages waiting for the radio, and whether each is bulk; the one being sent stays at the front
     * until it is out.
     */
    bricks::PacketQueue<config::lora::TxQueueDepth> m_TxQueue;
    Reassembler m_Reassembler;               ///< Puts received frames from the peer back together.
    std::array<uint8_t, 255> m_RxFrame {};   ///< Where one frame from the radio is read into.

    uint8_t m_Peer;                          ///< Address of the peer, LoraTransportConfig::PeerAddress.
    uint32_t m_TxCompleted  = 0;             ///< What TxCompleted() returns.
    ChannelNumber m_Channel = 0;             ///< Index into m_ChannelsMHz the radio is tuned to.

    /** @brief Who may transmit, and when to say over (Transport/TurnTaking.h). */
    TurnTaking m_Turns;
};
} // namespace ra::turtleford
