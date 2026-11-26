/**
 * @file Turtleford.cpp
 * @brief Implements Turtleford (Turtleford.h): the poll job on the work queue, the lock around the
 * public calls, and the listener that writes radio events to ra::Logger.
 */

#include "Turtleford.h"

#include <utility>

#include "Chrono.h"
#include "Log.h"

// The work queue's mode decides whether a poll can still be running on another thread when the
// destructor starts
#if defined(WORK_QUEUE_PREEMPTIVE) && WORK_QUEUE_PREEMPTIVE
    #define TURTLEFORD_PREEMPTIVE 1
    #include <chrono>
    #include <thread>
#else
    #define TURTLEFORD_PREEMPTIVE 0
#endif

namespace ra::turtleford
{
namespace
{
#if TURTLEFORD_PREEMPTIVE
/** @brief Wait for the queue to let go of the poll job: config::turtleford::JobReleaseTimeoutMs. */
constexpr std::chrono::milliseconds JobReleaseTimeout {config::turtleford::JobReleaseTimeoutMs};
#endif

/**
 * @brief One entry in the communications part of the log, stamped with our own clock, if LogEvents is on.
 * @tparam A    The types of the values logged; as ra::Logger::Log() takes them.
 * @param Enabled TurtlefordConfig::LogEvents. Nothing is logged when false.
 * @param Level   Severity of the entry.
 * @param Args    What to log.
 */
template <typename... A>
void LogIf(bool Enabled, Logger::Severity Level, A&&... Args)
{
    if (!Enabled) { return; }

    const Logger::LogInfo Entry {.Timestamp = static_cast<uint32_t>(hal::SysUptimeMs()),
                                 .Level     = Level,
                                 .Category  = type::Category::Communications};
    Logger::Instance().Log(Entry, std::forward<A>(Args)...);
}
} // namespace

Turtleford::Turtleford(hal::WorkQueue& Queue, IRadioListener& Listener, const TurtlefordConfig& Config) :
    m_Queue(Queue), m_Listener(Listener, Config.LogEvents), m_Radio(m_Listener, Config.Radio)
{
    hal::WorkQueue::SubmitOptions Options {};
    Options.Exec.PriorityValue = Config.PollPriority;
    Options.Exec.Fn            = &Turtleford::PollJob;
    Options.Exec.Ctx           = this;
    Options.Sched.DelayMs      = Config.PollIntervalMs;
    Options.Sched.Iterations   = hal::WorkQueue::Scheduling::IterationInfinite;

    auto [Status, Handle] = m_Queue.Submit(Options);
    if (Status == hal::WorkQueue::Status::Success) { m_Job = Handle; }
}

Turtleford::~Turtleford()
{
    m_Queue.Cancel(m_Job);

#if TURTLEFORD_PREEMPTIVE
    // Cancel only marks the job: a poll that is running, or was just picked, still runs to the end.
    // The queue drops its hold on the job after that, and GetContext() then returns null.
    const auto Deadline = std::chrono::steady_clock::now() + JobReleaseTimeout;
    while (m_Job.GetContext() != nullptr && std::chrono::steady_clock::now() < Deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Waits out a poll that is in progress. If the wait above timed out the queue is not running
    // jobs at all; should it start again later, a poll could still begin after we are gone.
    const RecursiveLock::Guard Held {m_Lock};
#endif
}

// ---------------------------------------------------------------------------
// What goes into the log. Each of these is rare; nothing here runs per telemetry sample.
// ---------------------------------------------------------------------------

void detail::LoggingListener::OnMessage(const Message& Msg, const RxInfo& Info)
{
    // What the other end told us to do or to be, in its own message type
    if (const auto* Command = std::get_if<type::CommandMsg>(&Msg))
    {
        LogIf(m_Enabled, Logger::Severity::Info, *Command);
    }
    else if (const auto* Status = std::get_if<type::StatusMsg>(&Msg))
    {
        LogIf(m_Enabled, Logger::Severity::Info, *Status);
    }
    m_App.OnMessage(Msg, Info);
}

void detail::LoggingListener::OnFailed(Ticket Id)
{
    // status: the ticket Send() returned
    LogIf(m_Enabled, Logger::Severity::Warn, Id, "reliable message given up");
    m_App.OnFailed(Id);
}

void detail::LoggingListener::OnLinkChanged(TransportKind Kind, bool Up)
{
    // status: the transport, numbered as type::TransportKind
    LogIf(m_Enabled,
          Up ? Logger::Severity::Info : Logger::Severity::Warn,
          static_cast<uint32_t>(Kind),
          Up ? "link up" : "link down");
    m_App.OnLinkChanged(Kind, Up);
}

void detail::LoggingListener::OnPeerRestarted(TransportKind Kind)
{
    // status: the transport it was heard on, numbered as type::TransportKind
    LogIf(m_Enabled, Logger::Severity::Warn, static_cast<uint32_t>(Kind), "peer restarted");
    m_App.OnPeerRestarted(Kind);
}

void detail::LoggingListener::OnChannelChanged(TransportKind Kind, ChannelNumber Channel)
{
    // The same message that asks for a change says where we ended up
    LogIf(m_Enabled, Logger::Severity::Info, PbGen_SwitchChannelMsg(Kind, Channel));
    m_App.OnChannelChanged(Kind, Channel);
}

void Turtleford::PollJob(hal::WorkQueue::WorkHandle& Handle)
{
    auto* Self = static_cast<Turtleford*>(Handle.GetContext());
    if (Self == nullptr) { return; }

    const RecursiveLock::Guard Held {Self->m_Lock};
    Self->m_Radio.Poll(static_cast<uint32_t>(hal::SysUptimeMs()));
}

bool Turtleford::AddTransport(ITransport& Transport)
{
    const RecursiveLock::Guard Held {m_Lock};
    return m_Radio.AddTransport(Transport);
}

SendResult Turtleford::Send(const Message& Msg)
{
    const RecursiveLock::Guard Held {m_Lock};
    return m_Radio.Send(Msg);
}

SendResult Turtleford::Send(const Message& Msg, const Route& Path)
{
    const RecursiveLock::Guard Held {m_Lock};
    return m_Radio.Send(Msg, Path);
}

SendResult Turtleford::Command(type::CommandType Command) { return Send(type::CommandMsg {Command}); }

SendResult Turtleford::FlightState(type::FlightState State) { return Send(type::StatusMsg {.State = State}); }

SendResult Turtleford::Telemetry(const type::FlightData& Data) { return Send(Data); }

SendResult Turtleford::Debug(uint32_t Status, std::string_view Text)
{
    return Send(type::DebugMsg {.Status = Status, .Text = Text});
}

bool Turtleford::RequestChannel(TransportKind Kind, ChannelNumber Channel)
{
    const RecursiveLock::Guard Held {m_Lock};
    return m_Radio.RequestChannel(Kind, Channel);
}

bool Turtleford::IsLinkUp(TransportKind Kind) const
{
    const RecursiveLock::Guard Held {m_Lock};
    return m_Radio.IsLinkUp(Kind);
}

ChannelNumber Turtleford::Channel(TransportKind Kind) const
{
    const RecursiveLock::Guard Held {m_Lock};
    return m_Radio.Channel(Kind);
}

size_t Turtleford::PendingReliable() const
{
    const RecursiveLock::Guard Held {m_Lock};
    return m_Radio.PendingReliable();
}

RadioStats Turtleford::Stats() const
{
    const RecursiveLock::Guard Held {m_Lock};
    return m_Radio.Stats();
}

bool Turtleford::IsRunning() const { return m_Job.IsAlive(); }
} // namespace ra::turtleford
