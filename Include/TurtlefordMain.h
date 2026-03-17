#pragma once

#include "ProtoMain.pb.h"
#include "WorkQueue.h"
#include "RadioCmdReciever.h"
#include "Transport/TransferSession.h"

namespace ra::turtleford
{

class TurtlefordMain
{
public:
    TurtlefordMain(std::shared_ptr<ITransferSession> session,float initialRadioFrequency);

    void Update();

    std::function<void(float)> SwitchRadioFrequency;

    void SetRecieveDataCallback(std::function<void(Proto_InFlightData)> RecieveData);

    void SetDebugMsgCallback(std::function<void(std::unique_ptr<std::string>)> DebugMessage);

    void SetInfoExchangeCallback(std::function<void(Proto_InfoExchange)> InfoExchange);

    void SetSwitchFrequencyCallback(std::function<void(float)> SetFrequency);

    void SendCmnd(Proto_MainMessage msg);

private:
    hal::WorkQueue WorkQueue;

    RadioCmndReciever CommandSenderReciever;

    // ITransferManager& TransferManager;
};

}; // namespace ra::turtleford