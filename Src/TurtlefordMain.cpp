#include "TurtlefordMain.h"
// #include "ITelemetryRadio.h"

namespace ra::turtleford
{

void transferProocessWrapper(ra::hal::WorkQueue::WorkHandle& stuff)
{
    ITransferManager* manager = static_cast<ITransferManager*>(stuff.GetContext());
    manager->Process();
}
// TransferManager.CreateSession(std::make_unique<LoraTransferConfig>(2, 3)
TurtlefordMain::TurtlefordMain(std::shared_ptr<ITransferManager> manager, float initialRadioFrequency) : 
    CommandSenderReciever(manager->CreateSession(std::make_unique<LoraTransferConfig>(static_cast<uint8_t>(2), static_cast<uint8_t>(3))), initialRadioFrequency)
{
    WorkQueue.Init();

    CommandSenderReciever.SetWorkQueue(&WorkQueue);

    hal::WorkQueue::SubmitOptions subOps {};

    subOps.Exec.Ctx           = manager.get();
    subOps.Exec.PriorityValue = hal::WorkQueue::Priority::High;
    subOps.Exec.Fn            = transferProocessWrapper;

    subOps.Sched.Iterations = std::numeric_limits<uint32_t>::max();

    const auto handle = WorkQueue.Submit(subOps);
}

void TurtlefordMain::Update()
{
#if !(WORK_QUEUE_PREEMPTIVE)
    WorkQueue.Run();
#endif
}

void TurtlefordMain::SetRecieveDataCallback(std::function<void(Proto_InFlightData)> RecieveData)
{
    CommandSenderReciever.RecieveData = RecieveData;
}

void TurtlefordMain::SetDebugMsgCallback(std::function<void(std::unique_ptr<std::string>)> DebugMessage)
{
    CommandSenderReciever.DebugMessage = DebugMessage;
}

void TurtlefordMain::SetInfoExchangeCallback(std::function<void(Proto_InfoExchange)> InfoExchange)
{
    CommandSenderReciever.InfoExchange = InfoExchange;
}

void TurtlefordMain::SetSwitchFrequencyCallback(std::function<void(float)> SetFrequency)
{
    CommandSenderReciever.SwitchRadioFrequency = SetFrequency;
}

void TurtlefordMain::SendCmnd(Proto_MainMessage msg) { CommandSenderReciever.SendCmnd(msg); }

} // namespace ra::turtleford
