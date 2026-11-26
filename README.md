# Turtleford
Avionics connectivity

This project implements the higher level communication protocol between the rocket and the
ground station. It sits above the HAL radio drivers and below the application.

# Usage

```cpp
#include "Turtleford.h"
using namespace ra::turtleford;

struct App : IRadioListener
{
    void OnMessage(const Message& Msg, const RxInfo& Info) override { /* std::visit / std::get_if */ }
    void OnFailed(Ticket Id) override { /* a reliable message was given up on */ }
};

hal::WorkQueue Queue;                        // the application's scheduler; it calls Queue.Init() and runs it
HAL::RFM95Radio HalRadio {CS, INT};          // the application owns and initialises the HAL radio
HalRadio.begin();

// Addresses are fixed per device; the other end is built with the two swapped
LoraTransport Lora {HalRadio, {.ChannelsMHz = {915.0f, 917.5f, 920.0f}, .Address = 1, .PeerAddress = 2}};

App Listener;
// The ground station is the channel master (the rocket leaves it false): only the master may call RequestChannel
Turtleford Link {Queue, Listener, {.Radio = {.Seed = BootCounter, .Link = {.ChannelMaster = true}}}};   // starts polling itself
Link.AddTransport(Lora);

Link.Command(type::CommandType::CameraOn);                            // retried until acked, in order
Link.FlightState(type::FlightState::InFlight);                        // same: send once per change
Link.Telemetry(FlightData);                                           // sent when the radio has nothing else to send
```

`Turtleford` is a `Radio` plus a job on the application's `hal::WorkQueue` that calls `Radio::Poll`
every `PollIntervalMs` (5 by default), so the application never polls. When the queue is preemptive
(`WORK_QUEUE_PREEMPTIVE`, the host build) the listener's callbacks run on the queue's thread and a
lock makes every call safe from any thread; on the cooperative queue (Teensy) there is no lock and
everything runs in the context that runs the queue. It does not start or run the queue.

`Radio` can still be driven directly: construct it, `AddTransport`, `Send`, and call
`Radio.Poll(millis())` from your own loop or a simulated clock. The tests do this.

Retries, acknowledgements, ordering, duplicate removal, picking a transport, keepalives and
channel (frequency) changes are all internal. `Link.RequestChannel(TransportKind::Lora, 2)`
on the ground station moves both ends. It is a goal, not one attempt: if the ack is lost and both
ends fall back to the old channel it asks again by itself (`RadioConfig::ChannelRequestRetries`),
and `OnChannelChanged` reports the final outcome once.

# Structure

```
Turtleford       Include/Turtleford.h       a Radio plus a hal::WorkQueue job that polls it. What an app creates.
Radio            Include/Radio.h            typed messages, routing, timing. Can be driven directly with Poll().
 ├─ ProtoCodec   Include/ProtoCodec.h       type:: structs <-> protobuf (schema in Lib/Tisdale)
 ├─ Arq          Include/Link/ArqSender.h   one selective repeat sliding window, shared by all transports
 │              Include/Link/ArqReceiver.h (Link/Arq.h: how the two halves fit together)
 ├─ Link         Include/Link/Link.h        per transport: is the peer reachable, which channel
 │              Include/Link/ChannelRequest.h (the master's retries of a channel change)
 └─ ITransport   Include/Transport/         per radio type: chunking + the HAL driver
     └─ LoraTransport
         └─ TurnTaking  Include/Transport/TurnTaking.h  who may transmit on the half duplex link
```

Link's channel handling, ChannelRequest and TurnTaking are state machines built on
`bricks::StateMachine::NodeMachine` (`Lib/Carrot-River/DataStructure/StateMachine/NodeMachine.h`): each
state is a struct holding only its own data, and handles the events it reacts to.

* **One frame = one `Proto.MainMessage`.** The reliability fields (`epoch`, `peer_epoch`, `seq`,
  `ack`, `sack`, `oldest`) are part of that message, so the protocol is identical on every transport
  and an ack rides for free on whatever is sent next.
* **Reliable messages are one ordered stream, delivered once each.** A message that runs out of
  attempts is reported through `OnFailed` and skipped: the sender tells the receiver (`oldest`) to
  stop waiting for it, and the ones after it still arrive, in order. Nothing else is reset. The
  `epoch` changes only when an end reboots; then both directions start again, and a message that
  was received but not yet acked can arrive a second time, since the rebooted end remembers nothing.
* **Reliability lives above the transports.** A retransmission is routed again each time, so it
  moves to another transport when the first one dies, and the receiver still delivers it once.
* **Each message kind has a route** (`RadioConfig::Routes`): best effort or reliable, plus an
  ordered transport preference. `TransportKind::Any` in that list stands for every transport, so
  `{Ble, Any}` prefers BLE and falls back to the rest, `{Ble}` is BLE only, and the default `{Any}`
  takes whichever is available. A route can also be given per `Send`.
* **Telemetry waits for an idle radio.** On a `WhenIdle` best-effort route (the default for
  FlightData) a message waits in the `Radio` and is handed to its transport only when that
  transport has nothing queued or in flight, so the rocket's flight-state changes and its acks for
  ground commands always go first and never wait behind a pile of samples. Up to
  `config::radio::HeldPerKind` (4) of one kind wait, in order; once that many are waiting `Send` returns
  `Full` and the application decides what to do with the newest. Nothing accepted is dropped.
* **LoRa takes turns.** The radio is half duplex: while it sends it hears nothing, so a rocket that
  streams telemetry back to back would never hear the ground. `Radio` tells the transport which
  messages are telemetry (`bulk`: a stream, more right behind, nothing answers it). Whoever has said
  what it had to say marks its last message "over" (a flag in the last fragment) and stays off the
  air for `LoraTransportConfig::ListenMs` (150, plus up to a quarter at random). Any message that is
  the last in the queue says "over"; a telemetry stream says it once per `ListenEveryMs` (1000). A
  message that is not marked means "more to come", so the listener holds its own messages until it
  sees "over" (or the peer goes quiet, or 2 x `ListenEveryMs` have passed), then waits
  `ListenGuardMs` (10, at least the peer's poll interval: a radio only listens again when its
  application next calls `receive()`) and sends. A ground command therefore waits at most about
  `ListenEveryMs` plus one telemetry frame and goes out on the first attempt. The cost is
  `ListenMs` out of every `ListenEveryMs` of the telemetry rate (about 15 % at the defaults) and
  nothing while messages have gaps anyway. `ListenEveryMs = 0` turns it all off.
* **Transports only move whole messages.** Frame size, chunking and the channel-to-frequency
  table are their business. To add BLE or nRF, implement `ITransport` and `AddTransport` it.
* **No allocation after setup.** Every buffer is allocated in a constructor, sized from config.
  The ARQ windows are fixed arrays inside the `Radio` (Carrot-River's `CircularBuffer` for the
  sender, `config::arq::SenderCapacity` = 16 messages; `RingWindow` for the receiver, 17 frames): the config can only make them smaller.
* **Long debug text is split.** Text over 127 characters goes out, and into the log, as several
  messages; `chunks_left` counts down to 0 on the last one so a reader can join them and notice
  a missing part.
* **The radio events that matter go into the log.** `Turtleford` writes to `ra::Logger`, under
  category Communications: every command and flight state received (as that message), a reliable
  message that was given up on, the link going up or down, the peer rebooting (text, with the
  ticket or the transport as status) and a channel change (as a `SwitchChannel`). Telemetry,
  debug text, acks and keepalives are never logged, so the log grows with what happens and not
  with time. `TurtlefordConfig::LogEvents = false` turns it off. `Radio` on its own logs nothing.
  The logger writes to storage only when its 1 KiB cache (`config::logger::CacheSize`) fills, so the application calls
  `ra::Logger::Instance().Flush()` at the moments that matter (state changes, landing, power off).
* **`Radio` has no threads and no clock.** Everything runs inside `Poll(NowMs)`, which makes the
  whole stack testable against simulated lossy links (`Test/Sim/SimTransport.h`). `Turtleford`
  supplies the clock and the schedule, and a lock when the work queue is preemptive.

Each device has a fixed address (`LoraTransportConfig::Address`) and talks to exactly one other
(`PeerAddress`). The radio drops frames addressed to anyone else, and the transport drops frames
sent by anyone but the peer. This keeps other teams' traffic out; it is not authentication, since
anyone who knows the two addresses can still forge a frame.

Exactly one end must set `RadioConfig::Link.ChannelMaster` (the ground station); only it may
request channel changes. If the two ends ever end up on different channels, both search the
current, previous and home channel until they hear each other again. When that happens during a
`RequestChannel` the master takes the new channel as still wanted: once the link is found again it
repeats the request, `ChannelRequestRetries` times (default 2), and `RequestChannel` refuses a new
one until the outcome has been reported.

# Configuration

Every tunable value is in `Include/Config.h`, in one namespace per class that uses it:
`config::arq`, `config::link`, `config::lora`, `config::radio`, `config::turtleford` and
`config::logger` (all under `ra::turtleford`). Change a value there to change it everywhere.

* Defaults of the run-time config structs. Each field of `ArqSenderConfig`, `ArqReceiverConfig`,
  `LinkConfig`, `LoraTransportConfig`, `RadioConfig` and `TurtlefordConfig` takes its default from
  `Config.h`, for example `LoraTransportConfig::ListenMs = config::lora::ListenMs`. The structs stay
  in the headers of the classes they configure, and an application can still set any field when it
  builds the object, as in the usage example above.
* Compile-time values: capacities and buffer sizes (everything is allocated once, to these sizes),
  such as `config::arq::SenderCapacity` and `config::radio::HeldPerKind`, and a few internal timings.
  These can only be changed in `Config.h`.

The LoRa channel table defaults to `config::lora::ChannelsMHz`; an application sets its own in
`LoraTransportConfig::ChannelsMHz`. Both ends must be built with the same table: on air a channel is
only an index into it.

The default route of each message kind is `config::radio::Routes`. The route types (`Message`,
`Route`) are in `Include/Route.h`.

# build the project
Build the project with `./build.py` or `python build.py`
See `build.py -h`

# Diagram
See ["Software design/Connectivity"](https://drive.google.com/file/d/1tTC5mFJvyw5gBqShJE5eQrG5UV4tqmlZ/view?usp=sharing) in the Avionics google drive
