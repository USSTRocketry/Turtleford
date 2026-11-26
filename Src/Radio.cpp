/**
 * @file Radio.cpp
 * @brief Implements Radio (Radio.h): sending and routing, the Poll() loop, frame handling and link events.
 *
 * Poll() is the only place that moves time forward. It receives, gives up on timed out reliable
 * messages, runs the links, sends what is due and starts the retransmit timers of what has left.
 */

#include "Radio.h"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace ra::turtleford
{
namespace
{
/** @brief Largest epoch. Epochs stay below 2^14 so they cost two bytes on air. */
constexpr uint32_t EpochRange = 0x3FFF;

/**
 * @brief The seed this end draws from. The two ends must not draw the same numbers, or their retries
 * would stay in step. Exactly one of them is the channel master, so that separates them even when
 * both were given the same seed.
 * @param Config The Radio's config.
 */
uint32_t EndSeed(const RadioConfig& Config) { return Config.Seed * 2 + (Config.Link.ChannelMaster ? 1 : 0); }

/**
 * @brief The transport's round trip in ms. A transport that reports no round trip at all still gets a
 * timeout of 1 ms, not 0.
 * @param T The transport.
 */
uint32_t BaseRoundTripMs(const ITransport& T) { return std::max<uint32_t>(T.Caps().RoundTripMs, 1); }

/**
 * @brief Encode an application message into the wire message. Not yet stamped, numbered or linked.
 * @param Msg The message.
 */
Proto_MainMessage ToProto(const Message& Msg)
{
    return std::visit(
        [](const auto& Value) -> Proto_MainMessage
        {
            using T = std::decay_t<decltype(Value)>;

            if constexpr (std::is_same_v<T, type::FlightData>) { return PbGen_FlightData(Value); }
            else if constexpr (std::is_same_v<T, type::StatusMsg>) { return PbGen_Status(Value); }
            else if constexpr (std::is_same_v<T, type::CommandMsg>) { return PbGen_CommandMsg(Value.Command); }
            else {
                static_assert(std::is_same_v<T, type::DebugMsg>, "Message alternative without a codec");
                return PbGen_DebugMsg(Value.Status, Value.Text);
            }
        },
        Msg);
}
} // namespace

Radio::Radio(IRadioListener& Listener, const RadioConfig& Config) :
    m_Listener(Listener), m_Config(Config), m_Tx(Config.Sender), m_Rx(Config.Receiver), m_Random(EndSeed(Config))
{
}

bool Radio::AddTransport(ITransport& Transport)
{
    const auto Kind = Transport.Caps().Kind;
    // None and Any are not transports: a route could not tell this one apart from all of them
    if (Kind == TransportKind::None || Kind == TransportKind::Any) { return false; }
    // From inside Poll() (a listener callback) adding a slot could move the ones Poll() is using
    if (m_Polling) { return false; }
    if (Find(Kind) != nullptr) { return false; }

    Slot& S     = m_Slots.emplace_back();
    S.Transport = &Transport;
    S.Conn      = Link {Transport, m_Config.Link};
    // A frame has left once the transport's count passes the one it had when the frame was handed
    // over, so count from where the transport already is: it may have sent messages before
    S.Handed    = Transport.TxCompleted();
    S.KeepaliveMs = NextKeepaliveMs();
    return true;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

SendResult Radio::Send(const Message& Msg) { return Send(Msg, m_Config.Routes[Msg.index()]); }

SendResult Radio::Send(const Message& Msg, const Route& Path)
{
    if (const auto* Debug = std::get_if<type::DebugMsg>(&Msg);
        Debug != nullptr && DebugChunkCount(Debug->Text.size()) > 1)
    {
        return SendChunked(*Debug, Path);
    }

    return SendOne(ToProto(Msg), static_cast<MessageKind>(Msg.index()), Path);
}

SendResult Radio::SendOne(Proto_MainMessage Msg, MessageKind Kind, const Route& Path)
{
    const Proto_MainMessage Proto = Stamped(Msg);
    const size_t Size             = EncodedSize(Proto);
    if (const auto Refused = Refusal(Path, Size)) { return {.Status = *Refused}; }

    if (Path.Mode == Delivery::Reliable)
    {
        const Ticket Id = m_Tx.Submit(Outgoing {.Msg = Proto, .Path = Path});
        if (Id == InvalidTicket) { return {.Status = SendStatus::Full}; }
        return {.Status = SendStatus::Queued, .Id = Id};
    }

    if (Path.WhenIdle)
    {
        // Kept for PumpWhenIdle(). Whether a transport can take it is decided then, not now.
        if (!m_Held[static_cast<size_t>(Kind)].Queue(Held {.Msg = Proto, .Path = Path})) { return {.Status = SendStatus::Full}; }
        return {.Status = SendStatus::Sent};
    }

    // Full, not NoRoute: see Refusal()
    Slot* To = PickTransport(Path, Size);
    if (To == nullptr) { return {.Status = SendStatus::Full}; }

    return {.Status = Transmit(Proto, *To) ? SendStatus::Sent : SendStatus::Full};
}

SendResult Radio::SendChunked(const type::DebugMsg& Debug, const Route& Path)
{
    const size_t Count = DebugChunkCount(Debug.Text.size());

    // The first part is the largest: full text and the highest chunks_left
    const size_t Size = EncodedSize(PbGen_DebugChunk(Debug.Status, Debug.Text, 0));
    if (const auto Refused = Refusal(Path, Size)) { return {.Status = *Refused}; }

    // Room for every part is checked before any is handed over, so a text is never half sent
    if (Path.Mode == Delivery::Reliable)
    {
        if (Count > m_Tx.Capacity()) { return {.Status = SendStatus::TooLarge}; }
        if (Count > m_Tx.Free() || m_Texts.Size() == config::radio::MaxReliableTexts) { return {.Status = SendStatus::Full}; }

        ReliableText Text {};
        for (size_t Index = 0; Index < Count; ++Index)
        {
            Text.Last = m_Tx.Submit(Outgoing {.Msg = DebugPart(Debug, Index), .Path = Path});
            if (Index == 0) { Text.First = Text.Last; }
        }
        m_Texts.Queue(Text);
        return {.Status = SendStatus::Queued, .Id = Text.Last};
    }

    // Full, not NoRoute: see Refusal()
    Slot* To = PickTransport(Path, Size);
    if (To == nullptr) { return {.Status = SendStatus::Full}; }
    if (Count > To->Transport->TxFree()) { return {.Status = SendStatus::Full}; }

    for (size_t Index = 0; Index < Count; ++Index)
    {
        if (!Transmit(DebugPart(Debug, Index), *To)) { return {.Status = SendStatus::Full}; }
    }
    return {.Status = SendStatus::Sent};
}

Proto_MainMessage Radio::Stamped(Proto_MainMessage Msg) const
{
    Msg.timestamp = m_NowMs;
    return Msg;
}

Proto_MainMessage Radio::DebugPart(const type::DebugMsg& Debug, size_t Index) const
{
    return Stamped(PbGen_DebugChunk(Debug.Status, Debug.Text, Index));
}

bool Radio::RequestChannel(TransportKind Kind, ChannelNumber Channel)
{
    if (!m_Config.Link.ChannelMaster) { return false; }

    Slot* S = Find(Kind);
    if (S == nullptr || !S->Conn.HasChannels() || Channel >= S->Transport->Caps().ChannelCount) { return false; }
    if (S->Request.Active() || S->Conn.GetState() != Link::State::Stable) { return false; }
    if (Channel == S->Conn.Channel()) { return false; }

    const Ticket Id = SubmitSwitch(*S, Channel);
    if (Id == InvalidTicket) { return false; }

    S->Request.Begin(Channel, m_Config.ChannelRequestRetries, Id);
    return true;
}

Ticket Radio::SubmitSwitch(const Slot& At, ChannelNumber Channel)
{
    const Proto_MainMessage Proto = Stamped(PbGen_SwitchChannelMsg(At.Transport->Caps().Kind, Channel));

    const Route Path {.Mode = Delivery::Reliable, .Preference = m_Config.ChannelRoute.Preference};
    return m_Tx.Submit(Outgoing {.Msg = Proto, .Path = Path});
}

void Radio::SubmitOwedRequest(Slot& At)
{
    if (!At.Request.SubmitOwed()) { return; }

    const Ticket Id = SubmitSwitch(At, At.Request.Wanted());
    if (Id != InvalidTicket) { At.Request.Submitted(Id); }
}

// ---------------------------------------------------------------------------
// Poll
// ---------------------------------------------------------------------------

void Radio::Poll(uint32_t NowMs)
{
    // AddTransport() is refused until Poll() returns, however it returns
    struct PollScope
    {
        bool& Flag;
        ~PollScope() { Flag = false; }
    };
    m_Polling = true;
    const PollScope Scope {m_Polling};

    m_NowMs = NowMs;
    if (m_Epoch == 0) { m_Epoch = NextEpoch(); }

    // 1. Everything that arrived
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        Slot& S = m_Slots[i];
        for (int n = 0; n < config::radio::ReceivesPerPoll; ++n)
        {
            const auto Bytes = S.Transport->Receive(NowMs);
            if (Bytes.empty()) { break; }
            HandleReceived(S, Bytes);
        }
    }

    // 2. The oldest reliable message is given up on once its last attempt has timed out. Only that
    //    one fails: the peer is told to stop waiting for it (see Transmit) and the rest carry on.
    m_Tx.Abandon(NowMs,
                 [this](Ticket Id)
                 {
                     ++m_Stats.GivenUp;
                     m_NextSkipMs = m_NowMs; // tell the peer at once
                     HandleFailed(Id);
                 });

    // 3. Reachability and channels
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        Slot& S              = m_Slots[i];
        const bool CanRetune = S.Transport->TxIdle() && !m_Rx.AckPending();
        HandleLinkEvents(S, S.Conn.Poll(NowMs, CanRetune));
    }

    // 4. Whatever has to go out
    PumpChannelRequests();
    PumpReliable();
    PumpWhenIdle();
    PumpAcks();
    PumpKeepalives();

    // 5. Let the radios work
    for (size_t i = 0; i < m_Slots.size(); ++i) { m_Slots[i].Transport->Poll(NowMs); }

    // 6. Reliable messages that have now left a radio start waiting for their ack
    m_Tx.StartTimers(NowMs,
                     [this](uint8_t Via, uint32_t Mark) -> uint32_t
                     {
                         const ITransport& T = *m_Slots[Via].Transport;
                         const bool Left     = static_cast<int32_t>(T.TxCompleted() - Mark) >= 0;
                         if (!Left) { return 0; }

                         return m_Random.Jittered(BaseRoundTripMs(T), config::radio::RetransmitJitterDivisor);
                     });
}

void Radio::PumpReliable()
{
    // Each pass either marks a frame as sent (so it is no longer due) or stops
    while (const auto Due = m_Tx.NextDue(m_NowMs))
    {
        Proto_MainMessage Msg = Due->Payload->Msg;
        Msg.has_seq           = true;
        Msg.seq               = Due->Seq;

        // Chosen again on every attempt, so a retransmission moves to whichever transport is up
        const Route& Path = Due->Payload->Path;
        const size_t Size = EncodedSize(Msg);
        Slot* To          = PickTransport(Path, Size);
        if (To == nullptr && !ReadyFor(Path, Size))
        {
            // Every transport it may use is down. Waiting uses up its attempts at the pace of a
            // retransmission, so if they do not come back it is given up on like any other message
            // (OnFailed, then skipped) instead of holding up every message behind it for good. Those
            // go out meanwhile, and are delivered in order once it is either sent or given up on.
            m_Tx.MarkSent(Due->Seq, m_NowMs, RouteRoundTripMs(Path));
            continue;
        }
        // Otherwise wait: a retune is being held for, or the transport has no room just now
        if (To == nullptr || !Transmit(Msg, *To)) { break; }

        // The wait for its ack starts when it has left the transport, see StartTimers below
        m_Tx.MarkQueued(Due->Seq, m_NowMs, m_Config.TxStallMs, static_cast<uint8_t>(IndexOf(*To)), To->Handed);
        if (Due->Attempt > 0) { ++m_Stats.Retransmissions; }
    }
}

void Radio::PumpWhenIdle()
{
    // The kinds take turns: start after the last one sent. Fixed for the pass, so none is skipped.
    const size_t Count = m_Held.size();
    const size_t Start = m_HeldNext;
    for (size_t n = 0; n < Count; ++n)
    {
        const size_t Index = (Start + n) % Count;
        auto& Waiting      = m_Held[Index];
        const auto Oldest  = Waiting.Peek();
        if (!Oldest.has_value()) { continue; }
        const Held& Next = Oldest->get();

        // Only onto a transport with nothing queued or in flight: whatever was handed over earlier,
        // reliable frames of this very Poll included, has gone out first
        Slot* To = PickTransport(Next.Path, EncodedSize(Next.Msg));
        if (To == nullptr || !To->Transport->TxIdle() || !Transmit(Next.Msg, *To, true)) { continue; }

        Waiting.Dequeue();
        m_HeldNext = (Index + 1) % Count;
    }
}

void Radio::PumpChannelRequests()
{
    for (size_t i = 0; i < m_Slots.size(); ++i) { SubmitOwedRequest(m_Slots[i]); }
}

void Radio::PumpAcks()
{
    const bool AckDue  = m_Rx.AckPending() && Elapsed(m_NowMs, m_AckSinceMs) >= m_Config.AckDelayMs;
    const bool SyncDue = m_SyncOwed && TimeReached(m_NowMs, m_NextSyncMs);
    // Messages we gave up on and the peer may still wait for. Usually told on the next frame anyway;
    // this is for when nothing else goes out (its held messages wait for that, and are not resent).
    const bool SkipDue = m_Tx.SkipNotice().has_value() && TimeReached(m_NowMs, m_NextSkipMs);
    if (!AckDue && !SyncDue && !SkipDue) { return; }

    // Answer where the peer was last heard, else where its link is up; an ack is allowed out even
    // while a retune is held. Nowhere else: the peer would not hear it (see Transmit).
    Slot* To = (m_ReplySlot < m_Slots.size() && m_Slots[m_ReplySlot].Transport->IsReady()) ? &m_Slots[m_ReplySlot] : nullptr;
    for (size_t i = 0; To == nullptr && i < m_Slots.size(); ++i)
    {
        if (m_Slots[i].Transport->IsReady() && m_Slots[i].Conn.IsUp()) { To = &m_Slots[i]; }
    }
    if (To == nullptr) { return; }

    if (TransmitEmpty(*To) && SyncDue) { m_NextSyncMs = m_NowMs + config::radio::EpochSyncIntervalMs; }
}

void Radio::PumpKeepalives()
{
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        Slot& S = m_Slots[i];
        if (!S.Transport->IsReady() || S.Conn.Holding()) { continue; }

        const bool Silent = m_Config.KeepaliveMs != 0 && Elapsed(m_NowMs, S.LastSentMs) >= S.KeepaliveMs;
        if (S.ProbeOwed || Silent) { TransmitEmpty(S); }
    }
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

Radio::Slot* Radio::PickTransport(const Route& Path, size_t Size)
{
    const auto Usable = [Size](const Slot& S)
    { return S.Transport->IsReady() && !S.Conn.Holding() && S.Transport->Caps().MaxMessageSize >= Size; };

    // The route's list in order; Any tries every transport in the order they were added
    const auto Pick = [&](bool NeedUp) -> Slot*
    {
        const auto Fits = [&](const Slot& S) { return Usable(S) && (!NeedUp || S.Conn.IsUp()); };

        for (const auto Kind : Path.Preference)
        {
            if (Kind == TransportKind::None) { break; } // the list stops at the first None
            if (Kind == TransportKind::Any)
            {
                for (size_t i = 0; i < m_Slots.size(); ++i)
                {
                    if (Fits(m_Slots[i])) { return &m_Slots[i]; }
                }
                return nullptr; // nothing after Any can add a transport
            }
            if (Slot* S = Find(Kind); S != nullptr && Fits(*S)) { return S; }
        }
        return nullptr;
    };

    // 1. A transport on which the peer is reachable: a preferred one, else any other
    if (Slot* S = Pick(true)) { return S; }

    // 2. The peer has not been heard anywhere. Send anyway: somebody has to speak first,
    //    and a rocket must keep transmitting whether or not it can hear the ground.
    return Pick(false);
}

size_t Radio::EncodedSize(const Proto_MainMessage& Msg) const
{
    // Size with every link field at its largest, so the answer holds for any retransmission
    Proto_MainMessage Worst = Msg;
    Worst.epoch             = EpochRange;
    Worst.peer_epoch        = EpochRange;
    Worst.has_seq           = true;
    Worst.seq               = std::numeric_limits<uint16_t>::max();
    Worst.has_ack           = true;
    Worst.ack               = std::numeric_limits<uint16_t>::max();
    Worst.sack              = std::numeric_limits<uint16_t>::max();
    Worst.has_oldest        = true;
    Worst.oldest            = std::numeric_limits<uint16_t>::max();

    return ProtoEncode(Worst, {});
}

template <typename F>
void Radio::ForEachAllowed(const Route& Path, F&& Fn) const
{
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        const ITransport& T = *m_Slots[i].Transport;
        if (Allows(Path, T.Caps().Kind)) { Fn(T); }
    }
}

std::optional<SendStatus> Radio::CheckRoute(const Route& Path, size_t Size) const
{
    // Every attached transport the route may use, whether or not it can carry anything right now:
    // one that is down may come back, but one that is missing or too small never will, and a
    // reliable message queued for it would hold up every message behind it for good
    if (Size == 0) { return SendStatus::TooLarge; } // does not even encode

    bool Attached = false;
    bool Fits     = false;
    ForEachAllowed(Path,
                   [&](const ITransport& T)
                   {
                       Attached = true;
                       Fits     = Fits || T.Caps().MaxMessageSize >= Size;
                   });

    if (Fits) { return std::nullopt; }
    return Attached ? SendStatus::TooLarge : SendStatus::NoRoute;
}

std::optional<SendStatus> Radio::Refusal(const Route& Path, size_t Size) const
{
    if (const auto Problem = CheckRoute(Path, Size)) { return Problem; }
    if (!ReadyFor(Path, Size)) { return SendStatus::NoRoute; } // all it may use is down now
    return std::nullopt;
}

bool Radio::Allows(const Route& Path, TransportKind Kind)
{
    for (const auto Preferred : Path.Preference)
    {
        if (Preferred == TransportKind::None) { break; } // the list stops at the first None
        if (Preferred == TransportKind::Any || Preferred == Kind) { return true; }
    }
    return false;
}

bool Radio::ReadyFor(const Route& Path, size_t Size) const
{
    // Ready now, not merely attached; a retune being held for (Link::Holding) still counts as ready
    bool Ready = false;
    ForEachAllowed(Path,
                   [&](const ITransport& T) { Ready = Ready || (T.IsReady() && T.Caps().MaxMessageSize >= Size); });
    return Ready;
}

uint32_t Radio::RouteRoundTripMs(const Route& Path) const
{
    uint32_t RtoMs = 1;
    ForEachAllowed(Path, [&](const ITransport& T) { RtoMs = std::max(RtoMs, BaseRoundTripMs(T)); });
    return RtoMs;
}

bool Radio::Transmit(Proto_MainMessage Msg, Slot& To, bool Bulk)
{
    if (m_Epoch == 0) { m_Epoch = NextEpoch(); }

    Msg.epoch      = m_Epoch;
    Msg.peer_epoch = m_PeerEpoch;

    // Any frame going out carries the ack that is owed. It only counts as given (and the peer as
    // told our epoch) on a transport where the peer is being heard; a keepalive on one it cannot
    // hear, say BLE out of range, must not use up the ack the peer is waiting for.
    const bool Heard      = IndexOf(To) == m_ReplySlot || To.Conn.IsUp();
    const bool CarriesAck = m_Rx.AckPending();
    if (CarriesAck)
    {
        Msg.has_ack = true;
        Msg.ack     = m_Rx.AckNext();
        Msg.sack    = m_Rx.AckSack();
    }

    // Until an ack shows the peer has moved on, every frame says which of our messages it can stop
    // waiting for because we gave up on them
    const auto Skip = m_Tx.SkipNotice();
    if (Skip.has_value())
    {
        Msg.has_oldest = true;
        Msg.oldest     = *Skip;
    }

    const size_t Size = ProtoEncode(Msg, m_EncodeBuffer);
    if (Size == 0 || !To.Transport->Send({m_EncodeBuffer.data(), Size}, Bulk)) { return false; }

    // Told again after a round trip if no ack has shown it arrived by then
    if (Skip.has_value()) { m_NextSkipMs = m_NowMs + 2 * BaseRoundTripMs(*To.Transport); }

    ++To.Handed;
    To.LastSentMs  = m_NowMs;
    To.KeepaliveMs = NextKeepaliveMs();
    To.ProbeOwed  = false;
    if (Heard)
    {
        m_SyncOwed = false; // every frame tells the peer our epoch
        if (CarriesAck) { m_Rx.AckSent(); }
    }

    ++m_Stats.FramesSent;
    return true;
}

bool Radio::TransmitEmpty(Slot& To)
{
    return Transmit(Stamped(Proto_MainMessage_init_zero), To);
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

void Radio::HandleReceived(Slot& From, std::span<const std::byte> Bytes)
{
    const auto Decoded = ProtoDecode_MainMessage(Bytes);
    if (!Decoded.has_value() || Decoded->epoch == 0)
    {
        ++m_Stats.DecodeErrors;
        return;
    }

    const Proto_MainMessage& Msg = *Decoded;
    const auto Via               = From.Transport->Caps().Kind;

    ++m_Stats.FramesReceived;
    m_ReplySlot = IndexOf(From);
    HandleLinkEvents(From, From.Conn.OnHeard(m_NowMs));

    if (Msg.epoch != m_PeerEpoch)
    {
        // First contact, or the peer restarted: it remembers nothing, so begin again on both
        // directions. Our unfinished messages are kept and renumbered.
        const bool Restarted = m_PeerEpoch != 0;
        m_PeerEpoch          = Msg.epoch;
        m_Rx.Reset();
        m_Tx.Restart();
        if (Restarted) { m_Listener.OnPeerRestarted(Via); }
    }

    // seq and ack only mean something if the peer numbered them against our current epoch
    const bool InSync = Msg.peer_epoch == m_Epoch;
    if (!InSync) { m_SyncOwed = true; }

    if (InSync && Msg.has_ack)
    {
        m_Tx.OnAck(static_cast<SeqNum>(Msg.ack),
                   static_cast<uint16_t>(Msg.sack),
                   m_NowMs,
                   [this](Ticket Id) { HandleDelivered(Id); });
    }

    const bool WasPending = m_Rx.AckPending();
    const auto DeliverReliable = [this](const Incoming& In) { Dispatch(In.Msg, In.Via, true); };

    // The peer gave up on some of its messages: stop waiting for them, and release what was held
    // behind them. Before this frame's own message, which may be the next one after them.
    if (InSync && Msg.has_oldest) { m_Rx.OnSkip(static_cast<SeqNum>(Msg.oldest), DeliverReliable); }

    if (!Msg.has_seq) { Dispatch(Msg, Via, false); }
    // A reliable message from a peer that is out of sync is dropped unacknowledged;
    // the peer resends it once it has heard our epoch.
    else if (InSync)
    {
        const auto Result =
            m_Rx.OnFrame(static_cast<SeqNum>(Msg.seq), Incoming {.Msg = Msg, .Via = Via}, DeliverReliable);

        // A channel change sent again after we delivered it means our ack was lost, and the master
        // moves on the ack that does get through. Move with it, even if we already moved once and
        // came back. Not one that is only held behind a gap: it takes effect in order, when the
        // messages before it have arrived.
        const bool AlreadyDelivered = static_cast<SeqNum>(Msg.seq - m_Rx.AckNext()) >= 0x8000;
        if (Result == decltype(m_Rx)::Result::Duplicate && AlreadyDelivered &&
            Msg.which_message_type == Proto_MainMessage_switch_channel_tag)
        {
            FollowSwitch(Msg.message_type.switch_channel);
        }
    }

    if (!WasPending && m_Rx.AckPending()) { m_AckSinceMs = m_NowMs; }
}

void Radio::Dispatch(const Proto_MainMessage& Msg, TransportKind Via, bool Reliable)
{
    const RxInfo Info {
        .Timestamp = Msg.timestamp, .ChunksLeft = Msg.chunks_left, .Via = Via, .Reliable = Reliable};

    switch (Msg.which_message_type)
    {
        case Proto_MainMessage_flight_data_tag:
            m_Listener.OnMessage(Message {PbTo_FlightData(Msg.message_type.flight_data)}, Info);
            break;

        case Proto_MainMessage_status_tag:
            m_Listener.OnMessage(Message {PbTo_Status(Msg.message_type.status)}, Info);
            break;

        case Proto_MainMessage_command_tag:
            m_Listener.OnMessage(Message {PbTo_CommandMsg(Msg.message_type.command)}, Info);
            break;

        case Proto_MainMessage_debug_tag:
            m_Listener.OnMessage(Message {PbTo_DebugMsg(Msg.message_type.debug)}, Info);
            break;

        case Proto_MainMessage_switch_channel_tag:
        {
            // Only honoured when it arrived reliably (in order, from the master's window)
            if (Reliable) { FollowSwitch(Msg.message_type.switch_channel); }
        }
        break;

        default:
            // empty frame (ack, keepalive, probe) or a message this build does not know
            break;
    }
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void Radio::FollowSwitch(const Proto_SwitchChannel& Switch)
{
    // Only the master asks; the master itself never follows
    if (m_Config.Link.ChannelMaster) { return; }

    // Refused when nothing would change: already settled there, or the retune is still waiting
    if (Slot* S = Find(static_cast<TransportKind>(Switch.transport)); S != nullptr)
    {
        S->Conn.BeginSwitch(Switch.channel);
    }
}

void Radio::HandleLinkEvents(Slot& At, const Link::Events& Events)
{
    const auto Kind = At.Transport->Caps().Kind;

    if (Events.Probe) { At.ProbeOwed = true; }
    if (Events.WentUp) { m_Listener.OnLinkChanged(Kind, true); }
    if (Events.WentDown) { m_Listener.OnLinkChanged(Kind, false); }
    if (Events.ChannelSettled) { HandleChannelSettled(At); }
}

void Radio::HandleChannelSettled(Slot& At)
{
    const auto Kind          = At.Transport->Caps().Kind;
    const ChannelNumber Here = At.Conn.Channel();

    switch (At.Request.OnSettled(Here))
    {
        case ChannelRequest::Next::Report:
            m_Listener.OnChannelChanged(Kind, Here);
            break;

        case ChannelRequest::Next::Wait:
            break;

        case ChannelRequest::Next::Retry:
            // The reliable queue may be full: the retry is then owed, and PumpChannelRequests() submits it
            SubmitOwedRequest(At);
            break;
    }
}

void Radio::HandleDelivered(Ticket Id)
{
    if (Slot* S = RequestOwner(Id))
    {
        // The peer agreed to move and is on its way; follow it
        S->Request.Answered();
        // Refused only when the link is already settled there (a search got to it first). No
        // settle event will follow then, so close the request here.
        if (!S->Conn.BeginSwitch(S->Request.Wanted())) { HandleChannelSettled(*S); }
        return;
    }

    if (TakeTextPart(Id, false)) { return; }
    m_Listener.OnDelivered(Id);
}

void Radio::HandleFailed(Ticket Id)
{
    if (Slot* S = RequestOwner(Id))
    {
        // No ack: the peer either never got the request or moved and its ack was lost
        S->Request.Answered();
        S->Conn.SearchIncluding(S->Request.Wanted(), m_NowMs);

        // No search started: there is nowhere else the peer can be (a search may already have
        // taken us there). No settle will follow, so this is the request's outcome.
        if (S->Conn.GetState() == Link::State::Stable) { HandleChannelSettled(*S); }
        return;
    }

    if (TakeTextPart(Id, true)) { return; }
    m_Listener.OnFailed(Id);
}

Radio::Slot* Radio::RequestOwner(Ticket Id)
{
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        if (m_Slots[i].Request.Owns(Id)) { return &m_Slots[i]; }
    }
    return nullptr;
}

bool Radio::TakeTextPart(Ticket Id, bool Failed)
{
    // Only the oldest text can have a part reported: outcomes (delivered and failed) come in the order
    // the messages were submitted, so no part of a later text is reported before the oldest is done.
    // ArqSenderTest.OutcomesAreReportedInTheOrderTheMessagesWereSubmitted guards that assumption.
    const auto Oldest = m_Texts.Peek();
    if (!Oldest.has_value()) { return false; }

    // Its tickets were handed out one after the other (16 parts at most, no wrap in practice)
    ReliableText& Text = Oldest->get();
    if (Id - Text.First > Text.Last - Text.First) { return false; }

    Text.PartLost = Text.PartLost || Failed;
    if (Id != Text.Last) { return true; } // a part the application never saw

    const bool Lost = Text.PartLost;
    m_Texts.Dequeue();
    if (Lost) { m_Listener.OnFailed(Id); }
    else { m_Listener.OnDelivered(Id); }
    return true;
}

uint32_t Radio::NextEpoch()
{
    // Taken from the seed itself, not drawn from m_Random. A boot counter as the seed then gives
    // every boot a different epoch, 16383 boots in a row (EndSeed steps by 2, and 2 and EpochRange
    // share no factor), so the peer always notices a reboot; a drawn epoch repeats 1 boot in 16383.
    // A random seed still gives a random epoch. 1 .. EpochRange: 0 means "not heard yet".
    return EndSeed(m_Config) % EpochRange + 1;
}

uint32_t Radio::NextKeepaliveMs() { return m_Random.Jittered(m_Config.KeepaliveMs, config::radio::KeepaliveJitterDivisor); }

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Radio::Slot* Radio::Find(TransportKind Kind)
{
    // The same search as the const one, which does not modify what it returns
    return const_cast<Slot*>(std::as_const(*this).Find(Kind));
}

const Radio::Slot* Radio::Find(TransportKind Kind) const
{
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        if (m_Slots[i].Transport->Caps().Kind == Kind) { return &m_Slots[i]; }
    }
    return nullptr;
}

bool Radio::IsLinkUp(TransportKind Kind) const
{
    const Slot* S = Find(Kind);
    return S != nullptr && S->Conn.IsUp();
}

ChannelNumber Radio::Channel(TransportKind Kind) const
{
    const Slot* S = Find(Kind);
    return S != nullptr ? S->Conn.Channel() : 0;
}
} // namespace ra::turtleford
