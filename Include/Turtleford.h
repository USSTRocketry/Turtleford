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

using LogMsg = std::string_view;
struct SwitchFrequency
{
};

using TelemetryOps  = std::variant<LogMsg, type::FlightData, Proto_SwitchRadioFrequency>;
using RadioCallback = void (*)(TelemetryOps);

class Turtleford
{
public:
    bool Init(float RadioFrequency);

    void TelemetryOperation(const TelemetryOps& Msg);

#if !(WORK_QUEUE_PREEMPTIVE)
    void Process();
#endif

private:
    std::unique_ptr<LoraTransferManager> TransferManager;
    hal::WorkQueue WorkQueue;
    RadioCmndReciever CommandSenderReciever;
};

}; // namespace ra::turtleford
