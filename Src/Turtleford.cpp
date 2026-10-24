#include "Turtleford.h"

#include <memory.h>
// #include "ITelemetryRadio.h"

namespace ra::turtleford
{

void TransferProcTrampoline(hal::WorkQueue::WorkHandle& W)
{ static_cast<ITransferManager*>(W.GetContext())->Process(); }

bool Turtleford::Init(float RadioFrequency)
{
    WorkQueue.Init();

    // TODO : Radio pins
    TransferManager = std::make_unique<LoraTransferManager>(new HAL::RFM95Radio(1, 2));

    // TODO : fix addr
    const auto S {TransferManager->CreateSession(
        std::make_unique<LoraTransferConfig>(static_cast<uint8_t>(2), static_cast<uint8_t>(3)))};

    CommandSenderReciever.Init(std::move(S), RadioFrequency);
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

void Turtleford::TelemetryOperation(const TelemetryOps& Msg) { CommandSenderReciever.SendCmnd(Msg); }
} // namespace ra::turtleford
