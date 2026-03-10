#include "TurtlefordMain.h"
//#include "ITelemetryRadio.h"

namespace ra::turtleford{


void transferProocessWrapper(ra::hal::WorkQueue::WorkHandle &stuff){
    ITransferSession *manager =  static_cast<ITransferSession*>(stuff.GetContext());
    manager->Process();
}
// TransferManager.CreateSession(std::make_unique<LoraTransferConfig>(2, 3)
TurtlefordMain::TurtlefordMain(ITransferSession* session) : CommandSenderReciever(session)
{
    WorkQueue.Init();

    CommandSenderReciever.SetWorkQueue(&WorkQueue);

    hal::WorkQueue::SubmitOptions subOps{};

    subOps.Exec.Ctx = session;
    subOps.Exec.PriorityValue = hal::WorkQueue::Priority::High;
    subOps.Exec.Fn = transferProocessWrapper;

    subOps.Sched.Iterations = std::numeric_limits<uint32_t>::max();

    const auto handle = WorkQueue.Submit(subOps);
    
}

void TurtlefordMain::Update(){
#if !(WORK_QUEUE_PREEMPTIVE)
    WorkQueue.Run();
#endif
}

void TurtlefordMain::SetRecieveDataCallback(void (*RecieveData)(Proto_InFlightData)){
    CommandSenderReciever.RecieveData = RecieveData;
}

void TurtlefordMain::SetDebugMsgCallback(void (*DebugMessage)(std::unique_ptr<std::string>)){
    CommandSenderReciever.DebugMessage = DebugMessage;
}

void TurtlefordMain::SetInfoExchangeCallback(void (*InfoExchange)(Proto_InfoExchange)){
    CommandSenderReciever.InfoExchange = InfoExchange;
}

}

