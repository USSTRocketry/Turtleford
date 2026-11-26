/**
 * @file Turtleford.h
 * @brief Everything an application needs to talk over the radio, and the class that runs it.
 *
 * @code
 * hal::WorkQueue Queue;                    // the application's scheduler, started by the application
 * LoraTransport Lora {HalRadio, LoraCfg};
 * Turtleford Link {Queue, Listener, {.Radio = {.Seed = BootCounter}}};
 * Link.AddTransport(Lora);
 *
 * Link.Command(type::CommandType::CameraOn);   // retried until acked
 * @endcode
 *
 * Turtleford is a Radio (Radio.h) plus a job on the work queue that polls it every few
 * milliseconds, so the application never calls Poll() itself. Use Radio directly to drive the
 * stack from your own loop or a simulated clock.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Config.h"
#include "Lock.h"
#include "Radio.h"
#include "Transport/LoraTransport.h"
#include "Transport/Transport.h"
#include "Type.h"
#include "WorkQueue.h"

namespace ra::turtleford
{
/** @brief Settings of a Turtleford. Defaults are in Config.h, config::turtleford. */
struct TurtlefordConfig
{
    RadioConfig Radio {}; ///< Settings of the Radio that Turtleford runs.
    /** @brief How often the stack runs, in ms. Also the longest a retry or ack can be late. */
    uint32_t PollIntervalMs               = config::turtleford::PollIntervalMs;
    /** @brief Priority of the poll job on the work queue. */
    hal::WorkQueue::Priority PollPriority = config::turtleford::PollPriority;
    /**
     * @brief Write the radio events that matter to ra::Logger (category Communications): every
     * command and flight state received, a reliable message that was given up on, the link going
     * up or down, and a channel change. Telemetry, debug text, acks and keepalives are never
     * logged, so the log grows with what happens, not with time.
     * The Logger drops what it is given while it has no storage callback.
     */
    bool LogEvents                        = config::turtleford::LogEvents;
};

/** @brief Internals of Turtleford, not for applications. */
namespace detail
{
/**
 * @brief Stands between the Radio and the application's listener: logs what TurtlefordConfig::LogEvents
 * describes, then passes every call on unchanged.
 */
class LoggingListener final : public IRadioListener
{
public:
    /**
     * @brief Construct a listener that logs and forwards.
     * @param App     The application's listener. Not owned; must outlive this object.
     * @param Enabled False to forward without logging.
     */
    LoggingListener(IRadioListener& App, bool Enabled) : m_App(App), m_Enabled(Enabled) {}

    /**
     * @brief Logs a received command or flight state, then forwards.
     * @param Msg  The message.
     * @param Info How it arrived.
     */
    void OnMessage(const Message& Msg, const RxInfo& Info) override;
    /**
     * @brief Forwards without logging.
     * @param Id The ticket Radio::Send() returned.
     */
    void OnDelivered(Ticket Id) override { m_App.OnDelivered(Id); }
    /**
     * @brief Logs a reliable message that was given up on, then forwards.
     * @param Id The ticket Radio::Send() returned.
     */
    void OnFailed(Ticket Id) override;
    /**
     * @brief Logs the link going up or down, then forwards.
     * @param Kind The transport.
     * @param Up   Whether the peer is reachable now.
     */
    void OnLinkChanged(TransportKind Kind, bool Up) override;
    /**
     * @brief Logs a channel change, then forwards.
     * @param Kind    The transport.
     * @param Channel The channel it is on.
     */
    void OnChannelChanged(TransportKind Kind, ChannelNumber Channel) override;
    /**
     * @brief Logs that the peer restarted, then forwards.
     * @param Kind The transport the peer was heard on.
     */
    void OnPeerRestarted(TransportKind Kind) override;

private:
    IRadioListener& m_App; ///< The application's listener. Not owned.
    bool m_Enabled;        ///< TurtlefordConfig::LogEvents.
};
} // namespace detail

/**
 * @brief A Radio that polls itself from a hal::WorkQueue.
 *
 * Listener callbacks run inside the poll job, and may call this object's methods. On a preemptive
 * queue (WORK_QUEUE_PREEMPTIVE) that is the queue's thread, and every method may be called from
 * any thread. On a cooperative one everything runs in the context that runs the queue, there is
 * no locking, and the methods must be called from that context. On a preemptive queue a call can
 * wait for as long as one poll takes, which includes one frame's time on air: the HAL's send()
 * blocks for it.
 * The queue and the listener must outlive this object. It does not start or run the queue:
 * the application owns its scheduler.
 */
class Turtleford
{
public:
    /**
     * @brief Starts the poll job. If the queue refuses it, IsRunning() is false: messages are still
     * accepted, but nothing is sent, received or reported until something polls the radio.
     * @param Queue    The application's work queue. Must outlive this object.
     * @param Listener Receives every message and event. Must outlive this object.
     * @param Config   Copied.
     */
    Turtleford(hal::WorkQueue& Queue, IRadioListener& Listener, const TurtlefordConfig& Config = {});
    /** @brief Cancels the poll job. When it returns no poll is running and none will start. */
    ~Turtleford();

    Turtleford(const Turtleford&)            = delete; ///< Not copyable: the poll job refers to this object.
    Turtleford& operator=(const Turtleford&) = delete; ///< Not copyable.

    /**
     * @brief See Radio::AddTransport().
     * @param Transport Referenced, not copied. Must outlive this object.
     * @return As Radio::AddTransport().
     */
    bool AddTransport(ITransport& Transport);

    /**
     * @brief See Radio::Send().
     * @param Msg The message.
     * @return As Radio::Send().
     */
    SendResult Send(const Message& Msg);
    /**
     * @brief See Radio::Send(const Message&, const Route&).
     * @param Msg  The message.
     * @param Path The route to use instead of the configured one.
     * @return As Radio::Send().
     */
    SendResult Send(const Message& Msg, const Route& Path);

    /** @name Shorthands for Send() with the configured route of that kind. @{ */
    /**
     * @brief Send a type::CommandMsg.
     * @param Command The command.
     * @return As Send().
     */
    SendResult Command(type::CommandType Command);
    /**
     * @brief Send a type::StatusMsg with the flight state.
     * @param State The flight state.
     * @return As Send().
     */
    SendResult FlightState(type::FlightState State);
    /**
     * @brief Send telemetry.
     * @param Data The sample.
     * @return As Send().
     */
    SendResult Telemetry(const type::FlightData& Data);
    /**
     * @brief Send a type::DebugMsg. Text too long for one message is split, see Radio::Send().
     * @param Status An application defined status code sent with the text.
     * @param Text   The text. Copied.
     * @return As Send().
     */
    SendResult Debug(uint32_t Status, std::string_view Text);
    /** @} */

    /**
     * @brief See Radio::RequestChannel().
     * @param Kind    The transport to move.
     * @param Channel The wanted channel.
     * @return As Radio::RequestChannel().
     */
    bool RequestChannel(TransportKind Kind, ChannelNumber Channel);

    /**
     * @brief See Radio::IsLinkUp().
     * @param Kind The transport.
     */
    bool IsLinkUp(TransportKind Kind) const;
    /**
     * @brief See Radio::Channel().
     * @param Kind The transport.
     */
    ChannelNumber Channel(TransportKind Kind) const;
    /** @brief See Radio::PendingReliable(). */
    size_t PendingReliable() const;
    /** @brief A copy: the poll job keeps updating the counters. */
    RadioStats Stats() const;

    /** @brief The queue accepted the poll job and it has not been cancelled. */
    bool IsRunning() const;

private:
    /**
     * @brief The work queue job: takes the lock and calls Radio::Poll() with the uptime.
     * @param Handle The job's handle. Its context is the Turtleford; null once the queue has let go of the job.
     */
    static void PollJob(hal::WorkQueue::WorkHandle& Handle);

private:
    hal::WorkQueue& m_Queue;           ///< The application's work queue. Not owned.
    /** @brief Serialises the public calls with the poll job. */
    mutable RecursiveLock m_Lock;
    detail::LoggingListener m_Listener; ///< Between the Radio and the application's listener.
    Radio m_Radio;                      ///< The stack being run.
    hal::WorkQueue::WorkHandle m_Job;   ///< The poll job. Not alive if the queue refused it or it was cancelled.
};
} // namespace ra::turtleford
