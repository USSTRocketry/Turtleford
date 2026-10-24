#pragma once

#include <variant>

#include "Type.h"
#include "ProtoMain.pb.h"
#include "WorkQueue.h"
#include "RadioCmdReciever.h"
#include "Transport/TransferSession.h"
#include "Transport/LoraTransport.h"

namespace ra::turtleford
{
struct DebugMsg
{
};

using RadioCommand  = std::variant<DebugMsg, type::FlightData>;
using RadioCallback = void (*)(RadioCommand);

class Turtleford
{
public:
    Turtleford(float RadioFrequency);

    void SendCmnd(const RadioCommand& Msg);

#if !(WORK_QUEUE_PREEMPTIVE)
    void Process();
#endif

private:
    hal::WorkQueue WorkQueue;
    RadioCmndReciever CommandSenderReciever;
    std::unique_ptr<LoraTransferManager> TransferManager = std::make_unique<LoraTransferManager>(1, 2);
};

}; // namespace ra::turtleford
