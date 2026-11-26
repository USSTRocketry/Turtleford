// Tests of the Turtleford wrapper: a Radio that polls itself from a hal::WorkQueue.
//
// The wrapper runs on the real clock and, in the host build, on the queue's own thread, so these
// tests wait for conditions with a generous timeout (never a fixed sleep as the thing asserted)
// and every object the queue thread and the test thread both touch is guarded here:
//   - LockedListener copies what a listener is told under a mutex.
//   - AirTransport puts the two simulated ends of a link behind one mutex, because a Send on one
//     end from the test thread writes into the other end's inbox while the queue thread polls it.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Log.h"
#include "Mocks/MessageMatchers.h"
#include "Sim/Recorder.h"
#include "Sim/Samples.h"
#include "Sim/SimTransport.h"
#include "Turtleford.h"
#include "WorkQueue.h"

using namespace ra::turtleford;
using ra::type::CommandMsg;
using ra::type::CommandType;
using ra::type::DebugMsg;
using ra::type::StatusMsg;
using ra::type::FlightData;
using ra::type::FlightState;
using mocks::ArrivedReliably;
using mocks::CarriesFlightData;
using mocks::IsCommand;
using mocks::IsDebug;
using mocks::IsFlightState;
using sim::SampleFlightData;
using sim::SimTransport;
using testing::AllOf;
using testing::Each;
using testing::Field;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::Not;
using testing::UnorderedElementsAre;
using testing::UnorderedElementsAreArray;
using Received = sim::Recorder::Received;

namespace
{
constexpr auto WaitLimit = std::chrono::seconds(10);

// True as soon as Done() is; false if it still is not after WaitLimit
bool WaitFor(const std::function<bool()>& Done)
{
    const auto Deadline = std::chrono::steady_clock::now() + WaitLimit;
    while (!Done())
    {
        if (std::chrono::steady_clock::now() >= Deadline) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

RadioConfig FastConfig(bool Master, uint32_t Seed)
{
    RadioConfig Config {};
    Config.Seed                  = Seed;
    Config.KeepaliveMs           = 100;
    Config.AckDelayMs            = 20;
    Config.Link.ChannelMaster    = Master;
    Config.Link.LostTimeoutMs    = 5000;
    Config.Link.ConfirmTimeoutMs = 400;
    Config.Link.SearchDwellMs    = 200;
    return Config;
}

TurtlefordConfig FastTurtleford(bool Master, uint32_t Seed)
{
    TurtlefordConfig Config {};
    Config.Radio          = FastConfig(Master, Seed);
    Config.PollIntervalMs = 2;
    return Config;
}

bool Contains(const std::vector<Ticket>& Tickets, Ticket Id)
{
    return std::find(Tickets.begin(), Tickets.end(), Id) != Tickets.end();
}

// A listener the queue thread writes and the test thread reads
class LockedListener final : public IRadioListener
{
public:
    /** @brief Runs on the queue thread after each message is recorded, outside our lock. Set before starting. */
    std::function<void(const Message&)> OnMessageHook {};

    std::vector<Received> Messages() const
    {
        std::scoped_lock Lock(m_Mutex);
        return m_Rec.Messages;
    }
    std::vector<Ticket> Delivered() const
    {
        std::scoped_lock Lock(m_Mutex);
        return m_Rec.DeliveredTickets;
    }
    std::vector<Ticket> Failed() const
    {
        std::scoped_lock Lock(m_Mutex);
        return m_Rec.FailedTickets;
    }
    std::vector<std::pair<TransportKind, ChannelNumber>> ChannelChanges() const
    {
        std::scoped_lock Lock(m_Mutex);
        return m_Rec.ChannelChanges;
    }

    void OnMessage(const Message& Msg, const RxInfo& Info) override
    {
        {
            std::scoped_lock Lock(m_Mutex);
            m_Rec.OnMessage(Msg, Info);
        }
        if (OnMessageHook) { OnMessageHook(Msg); }
    }
    void OnDelivered(Ticket Id) override
    {
        std::scoped_lock Lock(m_Mutex);
        m_Rec.OnDelivered(Id);
    }
    void OnFailed(Ticket Id) override
    {
        std::scoped_lock Lock(m_Mutex);
        m_Rec.OnFailed(Id);
    }
    void OnChannelChanged(TransportKind Kind, ChannelNumber Channel) override
    {
        std::scoped_lock Lock(m_Mutex);
        m_Rec.OnChannelChanged(Kind, Channel);
    }

private:
    mutable std::mutex m_Mutex;
    sim::Recorder m_Rec;
};

// One end of a simulated link behind a mutex shared with the other end, counting its polls
class AirTransport final : public ITransport
{
public:
    AirTransport(SimTransport& Sim, std::mutex& Air) : m_Sim(Sim), m_Air(Air) {}

    uint32_t Polls() const { return m_Polls.load(); }

    const TransportCaps& Caps() const override { return m_Sim.Caps(); }
    bool IsReady() const override { return Locked([&] { return m_Sim.IsReady(); }); }
    bool Send(std::span<const std::byte> Message, bool Bulk) override { return Locked([&] { return m_Sim.Send(Message, Bulk); }); }
    size_t TxFree() const override { return Locked([&] { return m_Sim.TxFree(); }); }
    uint32_t TxCompleted() const override { return Locked([&] { return m_Sim.TxCompleted(); }); }
    bool TxIdle() const override { return Locked([&] { return m_Sim.TxIdle(); }); }
    void Poll(uint32_t NowMs) override
    {
        m_Polls.fetch_add(1);
        Locked([&] { m_Sim.Poll(NowMs); });
    }
    std::span<const std::byte> Receive(uint32_t NowMs) override { return Locked([&] { return m_Sim.Receive(NowMs); }); }
    bool SetChannel(ChannelNumber Channel) override { return Locked([&] { return m_Sim.SetChannel(Channel); }); }
    ChannelNumber Channel() const override { return Locked([&] { return m_Sim.Channel(); }); }

private:
    template <typename Fn>
    auto Locked(Fn&& Body) const -> decltype(Body())
    {
        std::scoped_lock Lock(m_Air);
        return Body();
    }

    SimTransport& m_Sim;
    std::mutex& m_Air;
    std::atomic<uint32_t> m_Polls {0};
};

// Where ra::Logger stores to during a test. The queue thread writes, the test thread reads.
class LogSink
{
public:
    static uint32_t Store(std::span<const std::byte> Data, void* Context)
    {
        auto* Self = static_cast<LogSink*>(Context);
        std::scoped_lock Lock(Self->m_Mutex);
        Self->m_Bytes.insert(Self->m_Bytes.end(), Data.begin(), Data.end());
        return static_cast<uint32_t>(Data.size());
    }

    // Everything logged so far, in order. The Logger only hands over what it has cached when
    // something does not fit, so an oversized block of zeros goes in first to push it out; the
    // zeros read back as empty entries and are left out.
    std::vector<Proto_LogMessage> Entries()
    {
        const std::vector<std::byte> Push(4096);
        ra::Logger::Instance().Log(std::span<const std::byte>(Push));

        std::scoped_lock Lock(m_Mutex);
        std::vector<Proto_LogMessage> Out;
        auto Remaining = std::span<const std::byte>(m_Bytes);
        while (!Remaining.empty())
        {
            const auto Frame = ProtoFrame_Read(Remaining);
            if (!Frame.has_value()) { break; }

            const auto Decoded = ProtoDecode_LogMessage(Frame->Payload);
            if (Decoded && !Frame->Payload.empty()) { Out.push_back(*Decoded); }
            Remaining = Remaining.subspan(Frame->BytesConsumed);
        }
        return Out;
    }

    void Clear()
    {
        std::scoped_lock Lock(m_Mutex);
        m_Bytes.clear();
    }

private:
    std::mutex m_Mutex;
    std::vector<std::byte> m_Bytes;
};

// A log entry whose message is of this kind (a Proto_MainMessage_..._tag)
auto IsEntryOf(pb_size_t Tag)
{
    return Field("main_message",
                 &Proto_LogMessage::main_message,
                 Field("which_message_type", &Proto_MainMessage::which_message_type, Tag));
}

// A debug text entry with this status and text
MATCHER_P2(IsTextEntry, Status, Text, "is a text entry with status " + testing::PrintToString(Status) + " saying \"" + Text + "\"")
{
    if (arg.main_message.which_message_type != Proto_MainMessage_debug_tag) { return false; }

    const auto Debug = PbTo_DebugMsg(arg.main_message.message_type.debug);
    return Debug.Status == static_cast<uint32_t>(Status) && Debug.Text == Text;
}

// Two ends ("Ground" is the channel master, "Rocket" the follower) on one work queue
class TurtlefordTest : public testing::Test
{
protected:
    void SetUp() override
    {
        // Both ends log into the one Logger; start every test with it empty
        ra::Logger::Instance().RegisterCallback(&LogSink::Store, &Log);
        Log.Entries();
        Log.Clear();

        SimTransport::Connect(GroundSim, RocketSim);
        GroundSim.DelayMs = 1;
        RocketSim.DelayMs = 1;
        ASSERT_EQ(Queue.Init(), ra::hal::WorkQueue::Status::Success);
    }

    void TearDown() override
    {
        // Wrappers before the queue they run on
        Rocket.reset();
        Ground.reset();
        Queue.Deinit();
        ra::Logger::Instance().RegisterCallback(nullptr, nullptr);
    }

    void StartGround()
    {
        Ground = std::make_unique<Turtleford>(Queue, GroundListener, FastTurtleford(true, 111));
        ASSERT_TRUE(Ground->AddTransport(GroundAir));
    }
    void StartRocket()
    {
        Rocket = std::make_unique<Turtleford>(Queue, RocketListener, FastTurtleford(false, 222));
        ASSERT_TRUE(Rocket->AddTransport(RocketAir));
    }
    void StartBoth()
    {
        StartGround();
        StartRocket();
    }

    // Keepalives introduce the two ends to each other; sends before that are not what is tested
    void WaitForLink()
    {
        ASSERT_TRUE(
            WaitFor([&] { return Ground->IsLinkUp(TransportKind::Lora) && Rocket->IsLinkUp(TransportKind::Lora); }));
    }

    // Outlives the queue: the poll jobs log into it
    LogSink Log;

    // The queue comes first: it must outlive everything that runs on it
    ra::hal::WorkQueue Queue;

    std::mutex Air;
    SimTransport GroundSim {TransportKind::Lora, 1};
    SimTransport RocketSim {TransportKind::Lora, 2};
    AirTransport GroundAir {GroundSim, Air};
    AirTransport RocketAir {RocketSim, Air};

    LockedListener GroundListener;
    LockedListener RocketListener;

    std::unique_ptr<Turtleford> Ground;
    std::unique_ptr<Turtleford> Rocket;
};
} // namespace

// ---------------------------------------------------------------------------
// Starting and delivering
// ---------------------------------------------------------------------------

TEST_F(TurtlefordTest, ConstructorStartsThePollJob)
{
    Turtleford Link {Queue, GroundListener};

    EXPECT_TRUE(Link.IsRunning());
}

TEST_F(TurtlefordTest, ReliableCommandIsDeliveredWithoutAnyExplicitPoll)
{
    StartBoth();
    WaitForLink();

    const auto Result = Ground->Command(CommandType::CameraOn);

    ASSERT_EQ(Result.Status, SendStatus::Queued);
    ASSERT_NE(Result.Id, InvalidTicket);
    ASSERT_TRUE(WaitFor([&] { return Contains(GroundListener.Delivered(), Result.Id); }));
    EXPECT_THAT(RocketListener.Messages(), ElementsAre(IsCommand(CommandType::CameraOn)));
    EXPECT_THAT(GroundListener.Failed(), IsEmpty());
    EXPECT_EQ(Ground->PendingReliable(), 0u);
}

TEST_F(TurtlefordTest, LinkStatsAndChannelAreReadableWhileItRuns)
{
    StartBoth();
    WaitForLink();

    EXPECT_TRUE(Ground->IsLinkUp(TransportKind::Lora));
    EXPECT_FALSE(Ground->IsLinkUp(TransportKind::Ble));
    EXPECT_EQ(Ground->Channel(TransportKind::Ble), 0);
    EXPECT_GT(Ground->Stats().FramesSent, 0u);
    EXPECT_GT(Rocket->Stats().FramesReceived, 0u);
}

TEST_F(TurtlefordTest, MasterCanRequestAChannelChange)
{
    StartBoth();
    WaitForLink();

    EXPECT_FALSE(Rocket->RequestChannel(TransportKind::Lora, 2)); // only the master may
    ASSERT_TRUE(Ground->RequestChannel(TransportKind::Lora, 2));

    ASSERT_TRUE(WaitFor([&] { return !GroundListener.ChannelChanges().empty(); }));
    EXPECT_THAT(GroundListener.ChannelChanges(), ElementsAre(testing::Pair(TransportKind::Lora, 2)));
    EXPECT_EQ(Ground->Channel(TransportKind::Lora), 2);
    ASSERT_TRUE(WaitFor([&] { return Rocket->Channel(TransportKind::Lora) == 2; }));
}

// ---------------------------------------------------------------------------
// Shorthands
// ---------------------------------------------------------------------------

TEST_F(TurtlefordTest, CommandShorthandSendsTheSameAsSend)
{
    StartBoth();
    WaitForLink();

    const auto Short = Ground->Command(CommandType::Abort);
    const auto Long  = Ground->Send(CommandMsg {CommandType::Abort});

    EXPECT_EQ(Short.Status, Long.Status);
    ASSERT_TRUE(WaitFor([&] { return RocketListener.Messages().size() >= 2; }));
    EXPECT_THAT(RocketListener.Messages(), ElementsAre(IsCommand(CommandType::Abort), IsCommand(CommandType::Abort)));
}

TEST_F(TurtlefordTest, FlightStateShorthandSendsTheSameAsSend)
{
    StartBoth();
    WaitForLink();
    const std::optional<FlightState> Wanted {FlightState::InFlight};

    const auto Short = Ground->FlightState(FlightState::InFlight);
    const auto Long  = Ground->Send(StatusMsg {.State = FlightState::InFlight});

    EXPECT_EQ(Short.Status, Long.Status);
    ASSERT_TRUE(WaitFor([&] { return RocketListener.Messages().size() >= 2; }));
    EXPECT_THAT(RocketListener.Messages(), ElementsAre(IsFlightState(Wanted), IsFlightState(Wanted)));
}

TEST_F(TurtlefordTest, TelemetryShorthandSendsTheSameAsSend)
{
    StartBoth();
    WaitForLink();
    const auto Data = SampleFlightData();

    // Let the first arrive before sending the second, so each arrival can be matched to its call
    const auto Short = Ground->Telemetry(Data);
    ASSERT_TRUE(WaitFor([&] { return RocketListener.Messages().size() >= 1; }));
    const auto Long = Ground->Send(Data);
    ASSERT_TRUE(WaitFor([&] { return RocketListener.Messages().size() >= 2; }));

    EXPECT_EQ(Short.Status, Long.Status);
    EXPECT_THAT(RocketListener.Messages(), ElementsAre(CarriesFlightData(Data), CarriesFlightData(Data)));
}

TEST_F(TurtlefordTest, DebugShorthandSendsTheSameAsSend)
{
    StartBoth();
    WaitForLink();

    const auto Short = Ground->Debug(7, "igniter continuity ok");
    const auto Long  = Ground->Send(DebugMsg {.Status = 7, .Text = "igniter continuity ok"});

    EXPECT_EQ(Short.Status, Long.Status);
    ASSERT_TRUE(WaitFor([&] { return RocketListener.Messages().size() >= 2; }));
    EXPECT_THAT(RocketListener.Messages(),
                ElementsAre(IsDebug(7, "igniter continuity ok"), IsDebug(7, "igniter continuity ok")));
}

TEST_F(TurtlefordTest, SendWithARouteOverridesTheConfiguredOne)
{
    StartBoth();
    WaitForLink();

    const auto Result = Ground->Send(CommandMsg {CommandType::CameraOff}, Route {.Mode = Delivery::BestEffort});

    EXPECT_EQ(Result.Status, SendStatus::Sent);
    ASSERT_TRUE(WaitFor([&] { return !RocketListener.Messages().empty(); }));
    EXPECT_THAT(RocketListener.Messages(), ElementsAre(AllOf(IsCommand(CommandType::CameraOff), Not(ArrivedReliably()))));
}

// ---------------------------------------------------------------------------
// Callbacks that call back in
// ---------------------------------------------------------------------------

TEST_F(TurtlefordTest, ListenerMayReplyFromInsideOnMessage)
{
    // The poll job holds the wrapper's lock while it calls OnMessage, which sends on the wrapper
    // again: that must not deadlock. A reliable reply and a best effort one, from both ends.
    std::atomic<Turtleford*> RocketLink {nullptr};
    std::atomic<Turtleford*> GroundLink {nullptr};
    RocketListener.OnMessageHook = [&](const Message& Msg)
    {
        const auto* Command = std::get_if<CommandMsg>(&Msg);
        if (Command != nullptr && Command->Command == CommandType::Abort)
        {
            RocketLink.load()->Command(CommandType::CameraOff);
            RocketLink.load()->Debug(5, "aborting");
        }
    };
    GroundListener.OnMessageHook = [&](const Message& Msg)
    {
        const auto* Command = std::get_if<CommandMsg>(&Msg);
        if (Command != nullptr && Command->Command == CommandType::CameraOff)
        {
            GroundLink.load()->Command(CommandType::SdLogOn);
        }
    };

    StartBoth();
    RocketLink = Rocket.get();
    GroundLink = Ground.get();
    WaitForLink();

    ASSERT_TRUE(Ground->Command(CommandType::Abort));

    ASSERT_TRUE(WaitFor([&] { return RocketListener.Messages().size() >= 2 && GroundListener.Messages().size() >= 2; }));
    EXPECT_THAT(RocketListener.Messages(), ElementsAre(IsCommand(CommandType::Abort), IsCommand(CommandType::SdLogOn)));
    // the two replies take different routes, so they may arrive in either order
    EXPECT_THAT(GroundListener.Messages(),
                UnorderedElementsAre(IsCommand(CommandType::CameraOff), IsDebug(5, "aborting")));
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

TEST_F(TurtlefordTest, EveryAcceptedReliableMessageIsReportedExactlyOnceWhileSendIsHammered)
{
    StartBoth();
    WaitForLink();

    constexpr int MinCalls    = 3000;
    constexpr size_t MinTaken = 300;
    const auto Deadline       = std::chrono::steady_clock::now() + WaitLimit;

    std::vector<Ticket> Accepted;
    int Calls = 0;
    while ((Calls < MinCalls || Accepted.size() < MinTaken) && std::chrono::steady_clock::now() < Deadline)
    {
        const auto Result = Ground->Command(static_cast<CommandType>(1 + (Calls % 7)));
        if (Result.Status == SendStatus::Queued) { Accepted.push_back(Result.Id); }
        else
        {
            ASSERT_EQ(Result.Status, SendStatus::Full); // the only refusal on a link that is up
            std::this_thread::yield();
        }

        if (Calls % 16 == 0) { Ground->Telemetry(SampleFlightData()); }
        ++Calls;
    }
    ASSERT_GE(Calls, MinCalls);
    ASSERT_GE(Accepted.size(), MinTaken);

    // Every accepted ticket ends as delivered or failed, and never both or twice
    ASSERT_TRUE(
        WaitFor([&] { return GroundListener.Delivered().size() + GroundListener.Failed().size() >= Accepted.size(); }));
    ASSERT_TRUE(WaitFor([&] { return Ground->PendingReliable() == 0; }));

    auto Reported     = GroundListener.Delivered();
    const auto Failed = GroundListener.Failed();
    Reported.insert(Reported.end(), Failed.begin(), Failed.end());
    EXPECT_THAT(Reported, UnorderedElementsAreArray(Accepted));

    // What the peer got is at least what was reported delivered
    size_t Commands = 0;
    for (const auto& Got : RocketListener.Messages()) { Commands += (Got.Kind == MessageKind::Command) ? 1 : 0; }
    EXPECT_GE(Commands, GroundListener.Delivered().size());
}

TEST_F(TurtlefordTest, NoPollIsMadeAfterTheDestructorReturns)
{
    // Each round builds, runs and destroys a wrapper with a fast poll, so a poll that the queue
    // had already picked when the destructor ran gets every chance to land late
    SimTransport Solo {TransportKind::Lora, 3};
    AirTransport SoloAir {Solo, Air};

    for (int Round = 0; Round < 25; ++Round)
    {
        TurtlefordConfig Config = FastTurtleford(true, 5);
        Config.PollIntervalMs   = 1;
        const uint32_t Before   = SoloAir.Polls();
        {
            Turtleford Link {Queue, GroundListener, Config};
            ASSERT_TRUE(Link.AddTransport(SoloAir)) << "round " << Round;
            ASSERT_TRUE(WaitFor([&] { return SoloAir.Polls() >= Before + 3; })) << "round " << Round;
        }
        const uint32_t AtDestruction = SoloAir.Polls();

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_EQ(SoloAir.Polls(), AtDestruction) << "round " << Round;
    }
}

TEST_F(TurtlefordTest, QueueKeepsWorkingAfterTheWrapperIsGone)
{
    auto Link = std::make_unique<Turtleford>(Queue, GroundListener);
    ASSERT_TRUE(Link->IsRunning());

    Link.reset();

    std::atomic<int> Ran {0};
    ra::hal::WorkQueue::SubmitOptions Options {};
    Options.Exec.Fn  = [](ra::hal::WorkQueue::WorkHandle& Handle)
    { static_cast<std::atomic<int>*>(Handle.GetContext())->fetch_add(1); };
    Options.Exec.Ctx = &Ran;
    ASSERT_EQ(Queue.Submit(Options).first, ra::hal::WorkQueue::Status::Success);
    EXPECT_TRUE(WaitFor([&] { return Ran.load() == 1; }));
}

// ---------------------------------------------------------------------------
// A queue that does not run the job
// ---------------------------------------------------------------------------

TEST_F(TurtlefordTest, SendStillWorksWhenTheQueueHasDroppedTheJob)
{
    // The mock queue accepts every valid job, even one that will never run, so the only way to
    // see a job that is not running is to take it away: Deinit() clears the queue
    Turtleford Link {Queue, GroundListener, FastTurtleford(true, 1)};
    ASSERT_TRUE(Link.IsRunning());

    Queue.Deinit();

    EXPECT_FALSE(Link.IsRunning());
    EXPECT_EQ(Link.Command(CommandType::Abort).Status, SendStatus::NoRoute);
    EXPECT_FALSE(Link.IsLinkUp(TransportKind::Lora));
    EXPECT_EQ(Link.PendingReliable(), 0u);
}

TEST_F(TurtlefordTest, QueueThatNeverRunsTheJobDoesNotHangTheDestructor)
{
    ra::hal::WorkQueue Idle; // never Init()ed: accepts the job, never runs it

    const auto Start = std::chrono::steady_clock::now();
    {
        Turtleford Link {Idle, GroundListener};
        EXPECT_TRUE(Link.IsRunning());
        EXPECT_EQ(Link.Command(CommandType::Abort).Status, SendStatus::NoRoute);
    }

    EXPECT_LT(std::chrono::steady_clock::now() - Start, std::chrono::seconds(10));
}

// ---------------------------------------------------------------------------
// What is written to the Logger
// ---------------------------------------------------------------------------

TEST_F(TurtlefordTest, CommandAndFlightStateReceivedAreLoggedOnceEachAsTheirOwnMessage)
{
    StartBoth();
    WaitForLink();

    const auto Command = Ground->Command(CommandType::CameraOn);
    const auto State   = Rocket->FlightState(FlightState::InFlight);
    ASSERT_TRUE(WaitFor([&] { return Contains(GroundListener.Delivered(), Command.Id); }));
    ASSERT_TRUE(WaitFor([&] { return Contains(RocketListener.Delivered(), State.Id); }));

    const auto Entries = Log.Entries();
    EXPECT_THAT(Entries, Each(Field("category", &Proto_LogMessage::category, Proto_Category_CATEGORY_COMMUNICATIONS)));
    EXPECT_THAT(Entries, testing::Contains(IsEntryOf(Proto_MainMessage_command_tag)).Times(1));
    EXPECT_THAT(Entries, testing::Contains(IsEntryOf(Proto_MainMessage_status_tag)).Times(1));

    for (const auto& Entry : Entries)
    {
        if (Entry.main_message.which_message_type == Proto_MainMessage_command_tag)
        {
            EXPECT_EQ(PbTo_CommandMsg(Entry.main_message.message_type.command).Command, CommandType::CameraOn);
        }
        if (Entry.main_message.which_message_type == Proto_MainMessage_status_tag)
        {
            EXPECT_EQ(PbTo_Status(Entry.main_message.message_type.status).State, FlightState::InFlight);
        }
    }
}

TEST_F(TurtlefordTest, TelemetryDebugTextAcksAndKeepalivesAreNotLogged)
{
    StartBoth();
    WaitForLink();

    for (int i = 0; i < 50; ++i)
    {
        Rocket->Telemetry(SampleFlightData()); // may be refused (Full) while samples wait for the radio
        ASSERT_TRUE(Rocket->Debug(7, "nothing to keep"));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(WaitFor([&] { return GroundListener.Messages().size() >= 20; }));

    // all that traffic, and the log holds only the two ends finding each other
    EXPECT_THAT(Log.Entries(),
                UnorderedElementsAre(IsTextEntry(TransportKind::Lora, "link up"), IsTextEntry(TransportKind::Lora, "link up")));
}

TEST_F(TurtlefordTest, ChannelChangeIsLoggedByBothEndsWithTheChannelTheyEndedOn)
{
    StartBoth();
    WaitForLink();

    ASSERT_TRUE(Ground->RequestChannel(TransportKind::Lora, 2));
    ASSERT_TRUE(WaitFor([&] { return !GroundListener.ChannelChanges().empty() && !RocketListener.ChannelChanges().empty(); }));

    const auto Entries = Log.Entries();
    EXPECT_THAT(Entries, testing::Contains(IsEntryOf(Proto_MainMessage_switch_channel_tag)).Times(2));
    for (const auto& Entry : Entries)
    {
        if (Entry.main_message.which_message_type != Proto_MainMessage_switch_channel_tag) { continue; }

        EXPECT_EQ(Entry.main_message.message_type.switch_channel.transport, Proto_Transport_TRANSPORT_LORA);
        EXPECT_EQ(Entry.main_message.message_type.switch_channel.channel, 2u);
    }
}

TEST_F(TurtlefordTest, ReliableMessageGivenUpIsLoggedWithItsTicket)
{
    // two attempts instead of six, so giving up takes a fraction of a second
    TurtlefordConfig Config          = FastTurtleford(true, 111);
    Config.Radio.Sender.MaxAttempts = 2;
    Ground                           = std::make_unique<Turtleford>(Queue, GroundListener, Config);
    ASSERT_TRUE(Ground->AddTransport(GroundAir));
    StartRocket();
    WaitForLink();

    {
        std::scoped_lock Lock(Air);
        GroundSim.Cut = true;
        RocketSim.Cut = true;
    }
    const auto Lost = Ground->Command(CommandType::CameraOn);
    ASSERT_EQ(Lost.Status, SendStatus::Queued);
    ASSERT_TRUE(WaitFor([&] { return Contains(GroundListener.Failed(), Lost.Id); }));

    const auto Entries = Log.Entries();
    EXPECT_THAT(Entries, testing::Contains(IsTextEntry(Lost.Id, "reliable message given up")).Times(1));
    EXPECT_THAT(Entries, Not(testing::Contains(IsEntryOf(Proto_MainMessage_command_tag)))) << "it never arrived";
}

TEST_F(TurtlefordTest, NothingIsLoggedWhenEventLoggingIsSwitchedOff)
{
    TurtlefordConfig GroundConfig = FastTurtleford(true, 111);
    TurtlefordConfig RocketConfig = FastTurtleford(false, 222);
    GroundConfig.LogEvents        = false;
    RocketConfig.LogEvents        = false;
    Ground                        = std::make_unique<Turtleford>(Queue, GroundListener, GroundConfig);
    Rocket                        = std::make_unique<Turtleford>(Queue, RocketListener, RocketConfig);
    ASSERT_TRUE(Ground->AddTransport(GroundAir));
    ASSERT_TRUE(Rocket->AddTransport(RocketAir));
    WaitForLink();

    const auto Command = Ground->Command(CommandType::CameraOn);
    ASSERT_TRUE(WaitFor([&] { return Contains(GroundListener.Delivered(), Command.Id); }));

    // the application still hears everything
    EXPECT_THAT(RocketListener.Messages(), ElementsAre(IsCommand(CommandType::CameraOn)));
    EXPECT_THAT(Log.Entries(), IsEmpty());
}
