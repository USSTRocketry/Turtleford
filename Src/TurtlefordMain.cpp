#include "TurtlefordMain.h"

namespace ra::turtleford{

TurtlefordMain::TurtlefordMain(){
    WorkQueue = hal::WorkQueue();
    WorkQueue.Init();

    std::shared_ptr<ra::turtleford::ITransferSession> session = TransferManager.CreateSession(std::make_unique<LoraTransferConfig>(2, 3, nullptr));

    CommandSenderReciever = RadioCmndReciever(session);

    hal::WorkQueue::SubmitOptions subOps;
    subOps.Exec.Ctx = this;
    subOps.Exec.PriorityValue = hal::WorkQueue::Priority::High;

    void TransferProcessWrapper(){TransferManager.Process();};

    subOps.Exec.Fn = ();
    WorkQueue.Submit(subOps);
}

void TurtlefordMain::Update(){
    WorkQueue.Run();
    //put in work queue
    //TransferManager.Process()
}


}

