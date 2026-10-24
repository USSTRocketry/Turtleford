#include "Turtleford.h"

#include <memory.h>
// #include "ITelemetryRadio.h"

namespace ra::turtleford
{

void TransferProcTrampoline(hal::WorkQueue::WorkHandle& W)
{ static_cast<ITransferManager*>(W.GetContext())->Process(); }

Turtleford::Turtleford(float RadioFrequency) :
    // TODO : fix addr
    CommandSenderReciever(TransferManager->CreateSession(
                              std::make_unique<LoraTransferConfig>(static_cast<uint8_t>(2), static_cast<uint8_t>(3))),
                          RadioFrequency)
{
    WorkQueue.Init();

    CommandSenderReciever.SetWorkQueue(&WorkQueue);

    // sched transfer manger job
    hal::WorkQueue::SubmitOptions SubOps {
        .Exec {.PriorityValue = hal::WorkQueue::Priority::High, .Fn = TransferProcTrampoline},
        .Sched {.Iterations = std::numeric_limits<uint32_t>::max()}
    };
    WorkQueue.Submit(SubOps);
}

#if !(WORK_QUEUE_PREEMPTIVE)
void Turtleford::Process() { WorkQueue.Run(); }
#endif

void Turtleford::SendCmnd(const RadioCommand& Msg) { CommandSenderReciever.SendCmnd(Msg); }

} // namespace ra::turtleford
