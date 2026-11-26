/**
 * @file Radio.h
 * @brief The public entry point: typed messages in, typed messages out.
 *
 * @code
 * LoraTransport Lora {HalRadio, LoraCfg};
 * Radio Radio {Listener, RadioCfg};
 * Radio.AddTransport(Lora);
 *
 * Radio.Send(type::CommandMsg {type::CommandType::CameraOn});   // retried until acked
 * Radio.Send(FlightData);                                       // goes out when the radio has nothing else to send
 *
 * loop: Radio.Poll(millis());
 * @endcode
 *
 * Everything else is internal: sequence numbers, acknowledgements, retransmission, ordering,
 * duplicate removal, choosing a transport, keepalives and channel changes.
 *
 * Layers, top to bottom:
 * @verbatim
 *   Radio          this class; codec, routing, timing
 *   ArqSender/ArqReceiver   one sliding window shared by every transport (Link/ArqSender.h, ArqReceiver.h)
 *   Link           per transport: is the peer reachable, which channel (Link/Link.h)
 *   ChannelRequest the master's retries of a channel change (Link/ChannelRequest.h)
 *   ITransport     per radio type: chunking and the hardware (Transport/Transport.h)
 * @endverbatim
 *
 * Threading: none. Send(), RequestChannel() and Poll() must be called from the same context.
 * Listener callbacks run inside Poll() and may call Send().
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "Config.h"
#include "DataStructure/Buffer/CircularBuffer.h"
#include "Link/ArqReceiver.h"
#include "Link/ArqSender.h"
#include "Link/ChannelRequest.h"
#include "Link/Link.h"
#include "ProtoCodec.h"
#include "Route.h"
#include "SmallRng.h"
#include "Transport/Transport.h"
#include "Type.h"

namespace ra::turtleford
{
/** @brief Identifies a reliable message to the application, in IRadioListener::OnDelivered() and OnFailed(). */
using Ticket = uint32_t;
/** @brief A ticket that never identifies a message. */
constexpr Ticket InvalidTicket = 0;

/** @brief What Radio::Send() did with a message. */
enum class SendStatus : uint8_t
{
    /** @brief Reliable: accepted, IRadioListener::OnDelivered() or OnFailed() will follow for the ticket. */
    Queued,
    /**
     * @brief Best effort: accepted, nothing further is reported. On a WhenIdle route it goes
     * out on a later Poll(), once its transport is idle.
     */
    Sent,
    /**
     * @brief Too many messages waiting, or the transport is held for a moment by a channel change;
     * try again later.
     */
    Full,
    /** @brief Does not fit any transport. */
    TooLarge,
    /** @brief No transport the route may use is attached, or all of them are down right now. */
    NoRoute,
};

/** @brief The answer of Radio::Send(). Converts to true when the message was accepted. */
struct SendResult
{
    SendStatus Status = SendStatus::NoRoute; ///< What happened to the message.
    Ticket Id         = InvalidTicket;       ///< Valid only when Status is SendStatus::Queued.

    /** @brief True for SendStatus::Queued and SendStatus::Sent. */
    explicit operator bool() const { return Status == SendStatus::Queued || Status == SendStatus::Sent; }
};

/** @brief What the Radio knows about a received message besides its contents. */
struct RxInfo
{
    uint32_t Timestamp; ///< Sender's clock when it was submitted, in ms.
    /**
     * @brief Parts of the same message still to come; 0 on the last part and on a message
     * that was not split. Parts arrive as separate OnMessage calls with the same Timestamp.
     * A part can be missing (lost on a best-effort route, or given up on): the count then skips
     * a value.
     */
    uint32_t ChunksLeft = 0;
    TransportKind Via;  ///< Transport the message arrived on.
    bool Reliable;      ///< Arrived on a reliable route: delivered once and in order. Otherwise best effort.
};

/** @brief Implemented by the application. All calls happen inside Radio::Poll(). */
class IRadioListener
{
public:
    virtual ~IRadioListener() = default;

    /**
     * @brief A message arrived. Data inside Msg is only valid during the call.
     * @param Msg  The message.
     * @param Info How it arrived.
     */
    virtual void OnMessage(const Message& Msg, const RxInfo& Info) = 0;

    /**
     * @brief The peer acknowledged a reliable message. The argument is the ticket Radio::Send() returned.
     * Does nothing by default.
     */
    virtual void OnDelivered(Ticket) {}
    /**
     * @brief A reliable message was given up on; the peer may or may not have received it. The argument
     * is the ticket Radio::Send() returned. Does nothing by default.
     */
    virtual void OnFailed(Ticket) {}

    /**
     * @brief The peer became reachable or unreachable on a transport. The arguments are the transport
     * and whether the peer is reachable now. Does nothing by default.
     */
    virtual void OnLinkChanged(TransportKind, bool /*Up*/) {}
    /**
     * @brief A channel change or a search for the peer finished on this channel. The arguments are
     * the transport and the channel it is on. Does nothing by default.
     *
     * For a RequestChannel() this is the final outcome and comes exactly once: the wanted channel
     * if the move worked, otherwise where the radio ended up once RadioConfig::ChannelRequestRetries
     * further attempts have failed. Attempts that fall back to the old channel are not reported.
     * Any other time the peer is found again (after losing it, or on the follower end) it is
     * reported as soon as it happens.
     */
    virtual void OnChannelChanged(TransportKind, ChannelNumber /*Channel*/) {}
    /**
     * @brief The peer rebooted: it was heard with a new epoch. It remembers nothing, so both
     * directions start again; a message it had received but not yet acked may arrive twice.
     * Reported on the first frame heard from it, on the transport that frame came in on.
     * The argument is that transport. Does nothing by default.
     */
    virtual void OnPeerRestarted(TransportKind) {}
};

/** @brief Settings of a Radio. Defaults are in Config.h, config::radio. */
struct RadioConfig
{
    /**
     * @brief Sets the link epoch and seeds the timing jitter. Should differ on every boot (boot
     * counter, ADC noise, RTC, ...) so the peer can tell that this end has restarted. The epoch is
     * derived from it directly, so consecutive values (a boot counter) always give different
     * epochs, up to 16383 boots in a row; a random value gives a random epoch. Set per boot, so it
     * has no entry in Config.h.
     */
    uint32_t Seed = 0;

    /** @brief Route per message kind, indexed by MessageKind. */
    std::array<Route, static_cast<size_t>(MessageKind::Count)> Routes = config::radio::Routes;
    /** @brief Transports the internal channel change request may use. It always travels reliably. */
    ChannelRequestRoute ChannelRoute = config::radio::ChannelRoute;
    /**
     * @brief Extra attempts after the first when a requested channel change does not end on the
     * wanted channel (the ack was lost and both ends fell back to the old channel). 0 means a
     * request is tried once and the app is told where the radio landed.
     */
    uint8_t ChannelRequestRetries = config::radio::ChannelRequestRetries;

    ArqSenderConfig Sender {};     ///< The sending half of the sliding window.
    ArqReceiverConfig Receiver {}; ///< The receiving half of the sliding window.
    /** @brief Applies to every transport. Set Link.ChannelMaster on exactly one end. */
    LinkConfig Link {};

    /**
     * @brief A reliable message is sent again if it has sat in a transport's queue this long
     * without going out. The normal retransmit timeout only starts once it has left the radio.
     */
    uint32_t TxStallMs = config::radio::TxStallMs;

    /** @brief An ack waits this long for an outgoing message to ride on before going alone. */
    uint32_t AckDelayMs = config::radio::AckDelayMs;
    /**
     * @brief Send an empty frame on a transport that has been silent this long. 0 disables.
     * A random extra is added each time (config::radio::KeepaliveJitterDivisor), and to every
     * retransmit timeout (config::radio::RetransmitJitterDivisor), so two ends that transmitted on
     * top of each other do not do so again on the next try.
     */
    uint32_t KeepaliveMs = config::radio::KeepaliveMs;
};

/** @brief Counters of what the Radio has done since it was constructed. */
struct RadioStats
{
    uint32_t FramesSent      = 0; ///< Frames handed to a transport, retransmissions, acks and keepalives included.
    uint32_t FramesReceived  = 0; ///< Frames that decoded and carried an epoch.
    uint32_t Retransmissions = 0; ///< Reliable messages sent again after the first attempt.
    uint32_t DecodeErrors    = 0; ///< Received frames that did not decode, or had no epoch.
    /**
     * @brief Reliable messages that ran out of attempts; the application's own are reported
     * through IRadioListener::OnFailed(), internal channel requests are not.
     */
    uint32_t GivenUp         = 0;
};

/**
 * @brief Typed messages over one or more transports: routing, reliability, link tracking and keepalives.
 *
 * Reliable messages share one sliding window (ArqSender, ArqReceiver) whatever transport they use.
 * Best effort messages are sent once. Each attached transport has a Link that tracks whether the peer
 * is reachable and which channel it is on. Everything runs inside Poll(); there are no threads.
 * See the file comment for the layers.
 */
class Radio
{
public:
    /**
     * @brief Construct a Radio with no transports.
     * @param Listener Receives every message and event. Must outlive the Radio.
     * @param Config   Copied.
     */
    explicit Radio(IRadioListener& Listener, const RadioConfig& Config = {});

    Radio(const Radio&)            = delete; ///< Not copyable: transports and the listener are referenced.
    Radio& operator=(const Radio&) = delete; ///< Not copyable.

    /**
     * @brief Register a transport. It must outlive the Radio. One per TransportKind.
     * The channel it is on at this point becomes its home channel.
     * It must have nothing queued when it is attached, and be used only through this Radio from
     * then on: its TxCompleted() count is how the Radio knows that a message has left.
     * Meant for setup. Refused (false) from inside a listener callback: Poll() is then working on
     * the transports, and adding one may move them.
     * @param Transport Referenced, not copied.
     * @return False, and nothing is attached, if the transport's kind is TransportKind::None or
     * TransportKind::Any, one of that kind is attached already, or this is called from inside Poll().
     */
    bool AddTransport(ITransport& Transport);

    /**
     * @brief Send using the configured route for this kind of message.
     *
     * A message goes only over the transports its route allows. It is refused at once with NoRoute
     * if none of them is attached or all of them are down right now, and with TooLarge if none of
     * them can carry it. A reliable message accepted while its transports were up, which then go
     * down, keeps using up its attempts and is given up on (OnFailed) if they do not come back.
     *
     * On a WhenIdle route (the default for FlightData) the message waits in the Radio and goes out
     * on a later Poll(), once its transport is idle; Full if config::radio::HeldPerKind of its kind are waiting.
     * A best effort message that does not wait (not WhenIdle) is also refused as Full while a
     * channel change holds the transport for a moment; a reliable one is accepted and waits.
     *
     * Debug text longer than one message holds is split into parts (see RxInfo::ChunksLeft).
     * Either every part is accepted or none is. On a reliable route each part travels as a
     * reliable message of its own, and the one ticket returned stands for the whole text:
     * OnDelivered once every part has arrived, OnFailed if any part was given up on (the parts
     * after it still arrive). Full also when config::radio::MaxReliableTexts texts are still on their way.
     *
     * @param Msg The message. Copied: nothing is referenced after the call returns.
     * @return What was done with it; SendResult::Id is the ticket of a reliable message.
     */
    SendResult Send(const Message& Msg);
    /**
     * @brief Send with an explicit route, overriding the configured one.
     * @param Msg  As in Send(const Message&).
     * @param Path The route to use instead of RadioConfig::Routes.
     * @return As in Send(const Message&).
     */
    SendResult Send(const Message& Msg, const Route& Path);

    /**
     * @brief Move a transport, and the peer's, to another channel. Channel master only.
     * Fire and forget: if the change does not work out (the ack is lost and both ends end up on
     * the old channel) the radio asks again by itself, RadioConfig::ChannelRequestRetries times.
     * The final outcome is reported once through IRadioListener::OnChannelChanged.
     * @param Kind    The transport to move.
     * @param Channel The wanted channel, an index into the transport's channel table.
     * @return False, and nothing is sent, if this end is not the master; there is no such
     * transport or channel; it is the current channel; a change is in progress (its retries
     * included) or the link is searching for the peer; or the reliable queue is full (that last
     * one clears by itself, try again later).
     */
    bool RequestChannel(TransportKind Kind, ChannelNumber Channel);

    /**
     * @brief Run the radio. Call as often as possible with a millisecond clock.
     * May block for as long as the underlying HAL blocks on one frame.
     * @param NowMs Current time in ms. Only differences matter, and it may wrap.
     */
    void Poll(uint32_t NowMs);

    /**
     * @brief Whether the peer is reachable on a transport.
     * @param Kind The transport.
     * @return False if there is no such transport.
     */
    bool IsLinkUp(TransportKind Kind) const;
    /**
     * @brief Current channel of a transport, 0 if there is no such transport.
     * @param Kind The transport.
     */
    ChannelNumber Channel(TransportKind Kind) const;
    /** @brief Reliable messages accepted but not yet acknowledged. */
    size_t PendingReliable() const { return m_Tx.Pending(); }
    /** @brief The counters, updated by Poll(). */
    const RadioStats& Stats() const { return m_Stats; }

private:
    /** @brief One attached transport and what the Radio tracks for it. */
    struct Slot
    {
        ITransport* Transport = nullptr; ///< The transport. Not owned.
        Link Conn {};                    ///< Whether the peer is reachable on it, and which channel it is on.
        uint32_t LastSentMs  = 0;        ///< When a frame was last handed to the transport, in Poll() time.
        uint32_t KeepaliveMs = 0;        ///< Silence allowed before the next keepalive, chosen anew per frame.
        uint32_t Handed      = 0;        ///< Messages the transport has accepted from us, wraps.
        ChannelRequest Request {};       ///< Master only: the RequestChannel() still being worked on.
        bool ProbeOwed = false;          ///< Link::Events::Probe was raised: PumpKeepalives() sends an empty frame.
    };

    /** @brief A message on a WhenIdle route, waiting for its transport to be idle. */
    struct Held
    {
        Proto_MainMessage Msg {}; ///< The protobuf message (not yet encoded), stamped when Send() accepted it.
        Route Path {};            ///< The route it was sent with.
    };

    /** @brief A reliable message in the sender, as ArqSender holds it. */
    struct Outgoing
    {
        Proto_MainMessage Msg; ///< The protobuf message (not yet encoded), stamped when Send() accepted it. The seq is added per attempt.
        Route Path;            ///< The route it was sent with; the transport is chosen again per attempt.
    };

    /** @brief A reliable message in the receiver, as ArqReceiver holds it until it can be delivered in order. */
    struct Incoming
    {
        Proto_MainMessage Msg; ///< The decoded message.
        TransportKind Via;     ///< The transport it arrived on.
    };

    /** @brief The tickets of one long text sent reliably, in a row. Only Last was handed to the application. */
    struct ReliableText
    {
        Ticket First  = InvalidTicket; ///< Ticket of the first part.
        Ticket Last   = InvalidTicket; ///< Ticket of the last part: the one the application knows.
        bool PartLost = false;         ///< A part before Last was given up on.
    };

private:
    /** @brief Value of m_ReplySlot before any frame has been received. */
    static constexpr size_t NoSlot = std::numeric_limits<size_t>::max();
    /**
     * @brief Position of a slot in m_Slots. Small: there is at most one slot per TransportKind.
     * @param S An element of m_Slots.
     */
    size_t IndexOf(const Slot& S) const { return static_cast<size_t>(&S - m_Slots.data()); }

    /**
     * @brief The slot of the attached transport of this kind.
     * @param Kind The transport kind.
     * @return Null if there is none.
     */
    Slot* Find(TransportKind Kind);
    /** @copydoc Find(TransportKind) */
    const Slot* Find(TransportKind Kind) const;
    /**
     * @brief Choose the transport a message goes out on now.
     *
     * Walks the route's preference list. Prefers a transport on which the peer is reachable; if there
     * is none, takes one on which it is not, since somebody has to speak first.
     *
     * @param Path The route.
     * @param Size Encoded size of the message in bytes, as EncodedSize() returns it.
     * @return A transport the route allows that is ready, not held for a retune and big enough.
     *         Null if there is none.
     */
    Slot* PickTransport(const Route& Path, size_t Size);

    /**
     * @brief Send one message that is not split into parts.
     * @param Msg  The encoded message, not yet stamped.
     * @param Kind The kind of the message, which picks the line in m_Held.
     * @param Path The route.
     * @return As Send().
     */
    SendResult SendOne(Proto_MainMessage Msg, MessageKind Kind, const Route& Path);
    /**
     * @brief Send a debug text that needs several parts. Every part is accepted or none is.
     * @param Debug The text.
     * @param Path  The route.
     * @return As Send().
     */
    SendResult SendChunked(const type::DebugMsg& Debug, const Route& Path);
    /**
     * @brief Part Index of a long debug text, stamped and ready to send.
     * @param Debug The text.
     * @param Index Part number, from 0.
     */
    Proto_MainMessage DebugPart(const type::DebugMsg& Debug, size_t Index) const;

    /**
     * @brief The message with the current time as its timestamp. Every function that builds or accepts an
     * outgoing message stamps it with this itself: SendOne, DebugPart, SubmitSwitch, TransmitEmpty.
     * @param Msg The message.
     */
    Proto_MainMessage Stamped(Proto_MainMessage Msg) const;

    /**
     * @brief Size in bytes of the message on air, counted with every link field at its largest so
     * the answer holds for any retransmission.
     * @param Msg The message.
     * @return 0 if it does not encode.
     */
    size_t EncodedSize(const Proto_MainMessage& Msg) const;
    /**
     * @brief Why a message of this size can never go out on this route, or nothing if it can.
     * @param Path The route.
     * @param Size Encoded size in bytes.
     * @return SendStatus::TooLarge or SendStatus::NoRoute, or nothing.
     */
    std::optional<SendStatus> CheckRoute(const Route& Path, size_t Size) const;
    /**
     * @brief Why a message of this size is refused on this route now: CheckRoute's answer, else NoRoute when
     * everything it may use is down. Nothing if it is accepted. After that only a retune can keep
     * every transport out of reach (it is held for a moment while a channel change waits for it to
     * drain), so a best effort send that finds PickTransport() empty answers Full, not NoRoute: the
     * link is up, try again later.
     * @param Path The route.
     * @param Size Encoded size in bytes.
     */
    std::optional<SendStatus> Refusal(const Route& Path, size_t Size) const;
    /**
     * @brief Whether the route's preference list names this transport kind, directly or as TransportKind::Any.
     * @param Path The route.
     * @param Kind The transport kind.
     */
    static bool Allows(const Route& Path, TransportKind Kind);
    /**
     * @brief Calls Fn for each attached transport the route allows.
     * @tparam F Callable as `Fn(const ITransport&)`.
     * @param Path The route.
     * @param Fn   Called once per allowed transport, in the order they were added.
     */
    template <typename F>
    void ForEachAllowed(const Route& Path, F&& Fn) const;
    /**
     * @brief A transport the route may use is up and big enough right now.
     * @param Path The route.
     * @param Size Encoded size in bytes.
     */
    bool ReadyFor(const Route& Path, size_t Size) const;
    /**
     * @brief The slowest round trip of the transports the route may use, in ms. At least 1.
     * @param Path The route.
     */
    uint32_t RouteRoundTripMs(const Route& Path) const;
    /**
     * @brief Add the link fields (epochs, owed ack, skip notice) to a message and hand it to a transport.
     * @param Msg  The message.
     * @param To   The transport's slot.
     * @param Bulk One of a continuous stream, see ITransport::Send().
     * @return False if the message did not encode or the transport did not accept it.
     */
    bool Transmit(Proto_MainMessage Msg, Slot& To, bool Bulk = false);
    /**
     * @brief Transmit() a frame that carries no message: an ack, keepalive or probe.
     * @param To The transport's slot.
     * @return As Transmit().
     */
    bool TransmitEmpty(Slot& To);

    /**
     * @brief Decode a received frame and act on it: link state, epochs, acks, then the message itself.
     * @param From  The slot of the transport it arrived on.
     * @param Bytes The frame. Frames that do not decode are counted in RadioStats::DecodeErrors.
     */
    void HandleReceived(Slot& From, std::span<const std::byte> Bytes);
    /**
     * @brief Hand a received message to the listener, or act on it if it is a channel change.
     * @param Msg      The message.
     * @param Via      The transport it arrived on.
     * @param Reliable Whether it arrived on the reliable path.
     */
    void Dispatch(const Proto_MainMessage& Msg, TransportKind Via, bool Reliable);
    /**
     * @brief Follow the channel master's request to move. Does nothing on the master itself.
     * @param Switch The request: the transport and the channel.
     */
    void FollowSwitch(const Proto_SwitchChannel& Switch);
    /**
     * @brief Act on what Link reported: a probe is owed, the peer went up or down, a channel settled.
     * @param At     The slot of the transport.
     * @param Events What Link::Poll() or Link::OnHeard() returned.
     */
    void HandleLinkEvents(Slot& At, const Link::Events& Events);
    /**
     * @brief The link settled on a channel: continue or finish the slot's channel request, and tell
     * the listener where the radio is when it is finished.
     * @param At The slot of the transport.
     */
    void HandleChannelSettled(Slot& At);
    /**
     * @brief Put a channel change request in the reliable queue.
     * @param At      The slot of the transport to move.
     * @param Channel The wanted channel.
     * @return The ticket of the request, InvalidTicket if the queue is full.
     */
    Ticket SubmitSwitch(const Slot& At, ChannelNumber Channel);
    /**
     * @brief Submit the retry the slot is owed, if the reliable queue has room for it now.
     * @param At The slot of the transport.
     */
    void SubmitOwedRequest(Slot& At);
    /**
     * @brief The slot whose channel change request put Id in the reliable queue.
     * @param Id A ticket.
     * @return Null if it is not a channel change request.
     */
    Slot* RequestOwner(Ticket Id);
    /**
     * @brief A reliable message was acknowledged: continue a channel request, or tell the listener.
     * @param Id The ticket of the message.
     */
    void HandleDelivered(Ticket Id);
    /**
     * @brief A reliable message was given up on: continue a channel request, or tell the listener.
     * @param Id The ticket of the message.
     */
    void HandleFailed(Ticket Id);
    /**
     * @brief A part of a long reliable text: reported as the whole text's outcome, or not at all.
     * @param Id     The ticket of a message that was delivered or given up on.
     * @param Failed True if it was given up on.
     * @return True if the ticket belongs to a long text and has been dealt with, so the listener
     *         must not be told about it separately.
     */
    bool TakeTextPart(Ticket Id, bool Failed);

    /** @brief Send the reliable messages that are due: first attempts and timed out ones. */
    void PumpReliable();
    /** @brief Move best effort messages waiting on WhenIdle routes to transports that are idle. */
    void PumpWhenIdle();
    /** @brief Submit the channel change retries that could not be submitted before for lack of room. */
    void PumpChannelRequests();
    /** @brief Send an empty frame if an ack, an epoch or a skip notice is due and no other frame carried it. */
    void PumpAcks();
    /** @brief Send an empty frame on each transport that is silent past its keepalive time or owes a probe. */
    void PumpKeepalives();
    /** @brief Our epoch, from the seed. 1 .. 0x3FFF; never 0. */
    uint32_t NextEpoch();
    /** @brief Silence allowed before the next keepalive in ms: the configured one plus a random extra. */
    uint32_t NextKeepaliveMs();

private:
    /** @brief Room for the link fields on top of the largest payload. */
    static constexpr size_t EncodeBufferSize = Proto_MainMessage_size;

    IRadioListener& m_Listener; ///< The application. Not owned.
    RadioConfig m_Config;       ///< As given to the constructor.

    /** @brief One per transport, in the order they were added. Filled once at setup, so a vector: no fixed cap. */
    std::vector<Slot> m_Slots;

    /** @brief WhenIdle messages waiting, oldest first, one line per kind. */
    std::array<bricks::CircularBuffer<Held, config::radio::HeldPerKind>, static_cast<size_t>(MessageKind::Count)> m_Held {};
    size_t m_HeldNext = 0; ///< Kind to look at first in PumpWhenIdle, so kinds take turns.

    ArqSender<Outgoing, config::arq::SenderCapacity> m_Tx; ///< Reliable messages going out.
    ArqReceiver<Incoming> m_Rx;                            ///< Reliable messages coming in.
    /**
     * @brief Long texts sent reliably and not yet finished, oldest first. Outcomes come in the order the
     * messages were sent (only the oldest is ever acked or given up on), so the oldest text is the
     * only one whose parts can be reported. ArqSenderTest.OutcomesAreReportedInTheOrderTheMessagesWereSubmitted
     * guards that.
     */
    bricks::CircularBuffer<ReliableText, config::radio::MaxReliableTexts> m_Texts {};

    uint32_t m_NowMs = 0;     ///< The time given to Poll() last.
    SmallRng m_Random;        ///< Timing jitter (the epoch comes from the seed, see NextEpoch).
    uint32_t m_Epoch     = 0; ///< Ours, never 0 once running.
    uint32_t m_PeerEpoch = 0; ///< 0 until the peer has been heard.

    uint32_t m_AckSinceMs = 0; ///< When the oldest unsent ack became pending.
    /**
     * @brief Index of the slot the last frame came in on, NoSlot before the first. An index, not a pointer:
     * adding a transport may move the slots.
     */
    size_t m_ReplySlot    = NoSlot;
    uint32_t m_NextSyncMs = 0; ///< Earliest time for the next epoch-only frame.
    uint32_t m_NextSkipMs = 0; ///< Earliest time to tell the peer again what we gave up on.

    std::array<std::byte, EncodeBufferSize> m_EncodeBuffer {}; ///< Where Transmit() encodes the frame.
    bool m_SyncOwed = false; ///< The peer does not know our epoch; fills the pad after m_EncodeBuffer.
    bool m_Polling  = false; ///< Inside Poll(), where AddTransport() is refused.
    RadioStats m_Stats {};   ///< Counters returned by Stats().
};
} // namespace ra::turtleford
